// tests/lib/kit.js -- the suite kit: named cases, a case filter, assertions that
// say what they saw, loud skips, and one summary line the runners understand.
//
//   import { suite } from "./kit.js";
//   const t = suite("csv");
//   t.test("quoted field keeps its comma", (a) => {
//       a.eq(parse('"a,b"')[0][0], "a,b");
//   });
//   t.test("async reader", async (a) => { a.ok(await read()); }, { timeoutMs: 5000 });
//   await t.run();
//
// Shared fixtures and ordered cases:
//   t.before(async () => { srv = start(); })   once, before the first selected
//       case; never under --list or when nothing is selected. If it throws, the
//       suite fails and no case runs.
//   t.after(() => { if (srv) srv.close(); })    after the last case; also under
//       --list, so it must cope with a fixture that never started.
//   t.test("b", fn, { dependsOn: ["a"] })        selecting b also runs a (and
//       what a depends on), in declaration order, reported as prerequisites.
//
// Arguments after the file name (./dynajs FILE --case=PAT):
//   --case=PAT   run only the cases whose name contains PAT; /re/flags is a
//                regular expression. Repeatable; a case runs if ANY matches.
//                A filter that selects nothing is a failure, never a pass,
//                unless --none-ok (a runner over several suites passes it
//                and fails only when no suite selected anything).
//   --list       print the case names and run nothing
//   --bail       stop at the first failing case
//   --verbose    one line per case, with its check count and time
//   --json       end with one machine-readable line: KIT {...}
// DYNA_LOG=<file> (the one log; run2.sh exports it) gets one line per case and section:
//   YYYY-MM-DD HH:MM:SS.mmm<TAB>case<TAB>END<TAB><suite> :: <case><TAB>ok|fail|skip|done|threw<TAB><secs>s
//
// A case FAILS when an assertion fails, when it throws, or when it outlives
// its timeoutMs. A failed assertion does not abort the case (the rest of it
// still reports), except a.must(), which does. a.skip(why) ends the case as a
// SKIP that is printed and counted: a skipped case is never a silent pass.
//
// The last line is "<suite>: P passed, F failed[, S skipped]"; any failure also
// throws, so the process exits non-zero. Lines starting with FAIL are what the
// parallel runner greps for.
//
// The kit's own flags (--case, --list, --bail, --verbose, --json, --none-ok)
// are removed from scriptArgs when the kit loads, so a suite that parses its
// own arguments or forwards them to a child sees only its own.
import { cpuNow } from "./cpuclock.js";
const __tlFile = await import("dyna:file").catch(() => null);
const __tlSys = await import("dyna:sys").catch(() => null);

const PERF_SKIP = Symbol.for("kit.perfSkip");
const ARGV = (globalThis.scriptArgs || []).slice(1);
if (Array.isArray(globalThis.scriptArgs)) {
    const sa = globalThis.scriptArgs;
    for (let i = sa.length - 1; i >= 1; i--) {
        const s = String(sa[i]);
        if (s === "--list" || s === "--bail" || s === "--verbose" || s === "--json" || s === "--none-ok" || s.startsWith("--case=")) sa.splice(i, 1);
        else if (s === "--case" && i + 1 < sa.length) sa.splice(i, 2);
    }
}
const now = () => (typeof performance === "object" ? performance.now() : Date.now());
const TIMELINE = __tlFile && __tlSys ? __tlSys.getEnv("DYNA_LOG") : null;
const stampMs = () => { const d = new Date(), p = (n, w = 2) => String(n).padStart(w, "0"); return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`; };
const timeline = (suite, kase, res, ms) => {
    if (!TIMELINE) return;
    try { __tlFile.writeFile(TIMELINE, stampMs() + "\tcase\tEND\t" + suite + " :: " + String(kase).replace(/[\t\n]/g, " ") + "\t" + res + "\t" + (ms / 1000).toFixed(3) + "s\n", { append: true }); } catch (e) {}
};

class SkipCase { constructor(why) { this.why = why; } }
class Abort { constructor(msg) { this.msg = msg; } }

function show(v) {
    if (typeof v === "string") return JSON.stringify(v.length > 160 ? v.slice(0, 160) + "..." : v);
    if (typeof v === "bigint") return v + "n";
    if (typeof v === "number" && Object.is(v, -0)) return "-0";
    if (typeof v === "function") return "[function " + (v.name || "anonymous") + "]";
    if (v instanceof Error) return v.name + ": " + v.message;
    if (ArrayBuffer.isView(v) && !(v instanceof DataView)) {
        const head = Array.prototype.slice.call(v, 0, 16).join(",");
        return v.constructor.name + "(" + v.length + ")[" + head + (v.length > 16 ? ",..." : "") + "]";
    }
    if (v && typeof v === "object") {
        try { const s = JSON.stringify(v); return s.length > 160 ? s.slice(0, 160) + "..." : s; }
        catch (e) { return Object.prototype.toString.call(v); }
    }
    return String(v);
}

function deepEqual(a, b, seen) {
    if (Object.is(a, b)) return true;
    if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    seen = seen || new Map();
    if (seen.get(a) === b) return true;
    seen.set(a, b);
    if (a instanceof Date) return a.getTime() === b.getTime();
    if (a instanceof RegExp) return String(a) === String(b);
    if (a instanceof ArrayBuffer) { a = new Uint8Array(a); b = new Uint8Array(b); }
    if (ArrayBuffer.isView(a)) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false;
        return true;
    }
    if (a instanceof Map) {
        if (a.size !== b.size) return false;
        for (const [k, v] of a) if (!b.has(k) || !deepEqual(v, b.get(k), seen)) return false;
        return true;
    }
    if (a instanceof Set) {
        if (a.size !== b.size) return false;
        for (const v of a) if (!b.has(v)) return false;
        return true;
    }
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
        if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
        if (!deepEqual(a[k], b[k], seen)) return false;
    }
    return true;
}

function errorMatches(e, want) {
    if (want === undefined) return true;
    if (typeof want === "function") return e instanceof want;
    if (want instanceof RegExp) return want.test(String(e && e.message !== undefined ? e.message : e));
    if (typeof want === "string") return String(e && e.message !== undefined ? e.message : e).includes(want);
    if (want && typeof want === "object") {
        if (want.type && !(e instanceof want.type)) return false;
        if (want.message && !errorMatches(e, want.message)) return false;
        if (want.code !== undefined && (!e || e.code !== want.code)) return false;
        return true;
    }
    return false;
}

function parseArgs(argv) {
    const o = { filters: [], list: false, bail: false, verbose: false, json: false, noneOk: false };
    for (let i = 0; i < argv.length; i++) {
        const s = String(argv[i]);
        let pat = null;
        if (s === "--list") o.list = true;
        else if (s === "--bail") o.bail = true;
        else if (s === "--verbose") o.verbose = true;
        else if (s === "--json") o.json = true;
        else if (s === "--none-ok") o.noneOk = true;
        else if (s.startsWith("--case=")) pat = s.slice(7);
        else if (s === "--case" && i + 1 < argv.length) pat = String(argv[++i]);
        if (pat !== null) {
            const m = /^\/(.*)\/([a-z]*)$/.exec(pat);
            o.filters.push(m ? { re: new RegExp(m[1], m[2]), src: pat } : { sub: pat, src: pat });
        }
    }
    return o;
}

export function suite(name, opts) {
    const cfg = parseArgs((opts && opts.argv) || ARGV);
    const cases = [], afters = [], befores = [];
    let ran = false;

    const wanted = (n) => cfg.filters.length === 0
        || cfg.filters.some((f) => (f.re ? f.re.test(n) : n.includes(f.sub)));

    function makeAsserts(c) {
        const fail = (msg) => { c.failed++; c.msgs.push(msg); console.log("FAIL: [" + c.name + "] " + msg); };
        const pass = () => { c.passed++; };
        const label = (m) => (m ? m + ": " : "");
        const a = {
            ok(cond, msg) { if (cond) pass(); else fail(msg || "expected a truthy value"); return !!cond; },
            must(cond, msg) { if (cond) { pass(); return; } fail(msg || "precondition failed"); throw new Abort(msg); },
            eq(got, want, msg) {
                if (Object.is(got, want)) { pass(); return true; }
                fail(label(msg) + "got " + show(got) + ", expected " + show(want)); return false;
            },
            ne(got, not, msg) {
                if (!Object.is(got, not)) { pass(); return true; }
                fail(label(msg) + "got " + show(got) + ", which is the value it must not be"); return false;
            },
            deep(got, want, msg) {
                if (deepEqual(got, want)) { pass(); return true; }
                fail(label(msg) + "got " + show(got) + ", expected " + show(want)); return false;
            },
            near(got, want, tol, msg) {
                const t = tol === undefined ? 1e-9 : tol;
                const good = Object.is(got, want) || Math.abs(got - want) <= t * Math.max(1, Math.abs(want));
                if (good) { pass(); return true; }
                fail(label(msg) + "got " + show(got) + ", expected " + show(want) + " within " + t + " (relative)"); return false;
            },
            match(str, re, msg) {
                if (re.test(String(str))) { pass(); return true; }
                fail(label(msg) + show(String(str)) + " does not match " + re); return false;
            },
            throws(fn, want, msg) {
                try { fn(); }
                catch (e) {
                    if (errorMatches(e, want)) { pass(); return e; }
                    fail(label(msg) + "threw " + show(e) + ", which is not the expected error"); return e;
                }
                fail(label(msg) + "did not throw"); return undefined;
            },
            async rejects(p, want, msg) {
                try { await (typeof p === "function" ? p() : p); }
                catch (e) {
                    if (errorMatches(e, want)) { pass(); return e; }
                    fail(label(msg) + "rejected with " + show(e) + ", which is not the expected error"); return e;
                }
                fail(label(msg) + "did not reject"); return undefined;
            },
            skip(why) { throw new SkipCase(why || "no reason given"); },
            note(msg) { if (cfg.verbose) console.log("  note [" + c.name + "] " + msg); },
            best(fn, reps) {
                let b = Infinity;
                for (let r = 0; r < (reps || 5); r++) { const t0 = cpuNow(); fn(); b = Math.min(b, cpuNow() - t0); }
                return b;
            },
        };
        return a;
    }

    function withTimeout(p, ms, c) {
        if (!ms || typeof setTimeout !== "function") return p;
        let timer;
        const bound = new Promise((_, rej) => {
            timer = setTimeout(() => rej(new Abort("timed out after " + ms + " ms")), ms);
        });
        return Promise.race([p, bound]).finally(() => clearTimeout(timer));
    }

    const api = {
        name,
        verbose: cfg.verbose,
        listing: cfg.list,
        test(caseName, fn, o) { cases.push({ name: String(caseName), fn, opts: o || {}, skip: null }); },
        skip(caseName, why) { cases.push({ name: String(caseName), fn: null, opts: {}, skip: why || "no reason given" }); },
        before(fn) { befores.push(fn); },
        after(fn) { afters.push(fn); },
        async run() {
            if (ran) throw new Error("kit: " + name + ".run() called twice");
            ran = true;
            const dup = new Set();
            for (const c of cases) {
                if (dup.has(c.name)) throw new Error("kit: " + name + " declares the case \"" + c.name + "\" twice");
                dup.add(c.name);
            }
            if (cfg.list) {
                console.log("");
                for (const c of cases) console.log(c.name);
                console.log(name + ": " + cases.length + " cases listed");
                for (const fn of afters) {
                    try { await fn(); } catch (e) { console.log("FAIL: " + name + ": cleanup threw " + show(e)); throw e; }
                }
                return;
            }
            const byName = new Map(cases.map((c) => [c.name, c]));
            for (const c of cases)
                for (const d of c.opts.dependsOn || [])
                    if (!byName.has(d)) throw new Error("kit: " + name + ": \"" + c.name + "\" depends on the undeclared case \"" + d + "\"");
            const need = new Set(cases.filter((c) => wanted(c.name)).map((c) => c.name));
            const asked = new Set(need);
            for (let i = cases.length - 1; i >= 0; i--)
                if (need.has(cases[i].name)) for (const d of cases[i].opts.dependsOn || []) need.add(d);
            const chosen = cases.filter((c) => need.has(c.name));
            const t0 = now();
            let passed = 0, failed = 0, skipped = 0, checks = 0;
            const failedNames = [];
            if (cfg.filters.length && chosen.length === 0 && cfg.noneOk)
                console.log(name + ": none of its " + cases.length + " cases selected");
            else if (cfg.filters.length && chosen.length === 0) {
                console.log("FAIL: " + name + ": --case " + cfg.filters.map((f) => f.src).join(" ") + " selects none of its " + cases.length + " cases (--list prints them)");
                failed = 1;
            }
            let beforeErr = null;
            if (asked.size)
                for (const fn of befores) {
                    try { await fn(); }
                    catch (e) { beforeErr = e; console.log("FAIL: " + name + ": before threw " + show(e) + (e && e.stack ? "\n" + String(e.stack).split("\n").slice(0, 6).join("\n") : "")); break; }
                }
            if (beforeErr) { failed++; failedNames.push("before"); }
            for (const c of beforeErr ? [] : chosen) {
                c.passed = 0; c.failed = 0; c.msgs = [];
                const c0 = now();
                let state = "ok", why = "";
                globalThis[PERF_SKIP] = null;
                if (c.skip !== null) { state = "skip"; why = c.skip; }
                else {
                    try {
                        const r = c.fn(makeAsserts(c));
                        if (r && typeof r.then === "function") await withTimeout(r, c.opts.timeoutMs, c);
                    } catch (e) {
                        if (e instanceof SkipCase) { state = "skip"; why = e.why; }
                        else if (e instanceof Abort) { if (!c.failed) { c.failed++; console.log("FAIL: [" + c.name + "] " + e.msg); } }
                        else {
                            c.failed++;
                            console.log("FAIL: [" + c.name + "] threw " + show(e) + (e && e.stack ? "\n" + String(e.stack).split("\n").slice(0, 6).join("\n") : ""));
                        }
                    }
                    if (state === "ok" && c.failed) state = "fail";
                    if (state === "ok" && c.passed === 0 && globalThis[PERF_SKIP]) { state = "skip"; why = globalThis[PERF_SKIP]; }
                    if (state === "ok" && c.passed === 0) {
                        state = "fail"; c.failed++;
                        console.log("FAIL: [" + c.name + "] made no assertion (a case that checks nothing cannot pass)");
                    }
                }
                const ms = now() - c0;
                timeline(name, c.name, state, ms);
                checks += c.passed + c.failed;
                if (state === "skip") { skipped++; console.log("SKIP: [" + c.name + "] " + why); }
                else if (state === "fail") { failed++; failedNames.push(c.name); }
                else passed++;
                if (cfg.verbose && state !== "skip")
                    console.log((state === "ok" ? "ok    " : "FAILED") + " " + c.name + " (" + (c.passed + c.failed) + " checks, " + ms.toFixed(1) + " ms)");
                if (state === "fail" && cfg.bail) break;
            }
            for (const fn of afters) {
                try { await fn(); } catch (e) { failed++; console.log("FAIL: " + name + ": cleanup threw " + show(e)); }
            }
            const left = cases.length - chosen.length;
            const prereq = chosen.length - asked.size;
            const line = name + ": " + passed + " passed, " + failed + " failed"
                + (skipped ? ", " + skipped + " SKIPPED" : "")
                + (left ? ", " + left + " not selected" : "")
                + (cfg.filters.length && prereq > 0 ? ", " + prereq + " run as prerequisites" : "")
                + " (" + checks + " checks, " + (now() - t0).toFixed(0) + " ms)";
            if (cfg.json)
                console.log("KIT " + JSON.stringify({ suite: name, passed, failed, skipped, notSelected: left, checks, failedCases: failedNames }));
            console.log(line);
            if (failed) throw new Error(name + ": " + failed + " failing case" + (failed === 1 ? "" : "s") + (failedNames.length ? " (" + failedNames.join("; ") + ")" : ""));
        },
    };
    return api;
}

// sections(): named cases for a suite written as top-level blocks with its own
// ok/eq helpers (the codemod in .agent-work/granularity wraps each block):
//
//   const S = sections("csv");
//   S.section("quoted field keeps its comma", () => { ...the old block... });
//   await S.section("async reader", async () => { ... });
//   ...the suite's own summary...
//   S.end();
//
// Sections run EAGERLY, in place, so setup statements between blocks keep
// their order. With no kit argument a section is just a call: same output,
// same exceptions, nothing added. With --case/--list/--bail/--verbose/--json a
// selected section's exception and every FAIL line it prints (through print or
// console.log) fail THAT case, the others still run, and end() prints the kit
// summary and throws on any failure. An unselected section is skipped, unless
// the suite was built with { chain: true } (its blocks depend on earlier ones):
// then it still runs, as setup, and is not reported.
// Under --list the suite's own output is muted from sections() on (also after
// end(), so shutdown code after it runs silently), and `listing` is true so code
// outside every section can stand aside (if (!S.listing)).
// Section names are unique and sections never nest: a second declaration of a
// name, or a section opened synchronously inside another one, throws.
// A sloppy-mode SCRIPT (with, legacy octal, sloppy arguments, global var
// semantics) cannot import the kit statically; it loads it with
//   import("../lib/kit.js").then(async ({ sections }) => { ...sections... });
// and keeps its sloppy bodies inside the section callbacks.
export function sections(name, opts) {
    const cfg = parseArgs((opts && opts.argv) || ARGV);
    const chain = !!(opts && opts.chain);
    const plain = !cfg.filters.length && !cfg.list && !cfg.verbose && !cfg.json && !cfg.bail;
    const wanted = (n) => cfg.filters.length === 0
        || cfg.filters.some((f) => (f.re ? f.re.test(n) : n.includes(f.sub)));
    const seen = new Set();
    let passed = 0, failed = 0, notSelected = 0, selected = 0, stopped = false, depth = 0;
    const failedNames = [];
    const t0 = now();
    const out = { print: globalThis.print, log: console.log };
    if (cfg.list) {
        if (typeof out.print === "function") globalThis.print = () => {};
        console.log = () => {};
    }

    function begin() {
        const st = { fails: 0, print: globalThis.print, log: console.log };
        const spy = (orig) => (...a) => {
            if (/^\s*FAIL([: ]|$)/.test(a.map(String).join(" "))) st.fails++;
            return orig(...a);
        };
        if (typeof st.print === "function") globalThis.print = spy(st.print);
        console.log = spy(st.log);
        return st;
    }
    function finish(st, caseName, err, c0) {
        if (typeof st.print === "function") globalThis.print = st.print;
        console.log = st.log;
        let bad = st.fails > 0;
        if (err !== undefined) {
            bad = true;
            console.log("FAIL: [" + caseName + "] threw " + show(err) + (err && err.stack ? "\n" + String(err.stack).split("\n").slice(0, 6).join("\n") : ""));
        }
        timeline(name, caseName, bad ? "fail" : "ok", now() - c0);
        if (bad) { failed++; failedNames.push(caseName); if (cfg.bail) stopped = true; }
        else passed++;
        if (cfg.verbose)
            console.log((bad ? "FAILED" : "ok    ") + " " + caseName + " (" + (now() - c0).toFixed(1) + " ms)");
    }

    return {
        name,
        listing: cfg.list,
        section(caseName, fn) {
            caseName = String(caseName);
            if (seen.has(caseName)) throw new Error("kit: " + name + " declares the section \"" + caseName + "\" twice");
            seen.add(caseName);
            if (depth > 0) throw new Error("kit: " + name + " nests the section \"" + caseName + "\" inside another section");
            if (plain && !TIMELINE) { depth++; try { return fn(); } finally { depth--; } }
            if (plain) {
                const c0 = now();
                let r;
                depth++;
                try { r = fn(); } catch (e) { timeline(name, caseName, "threw", now() - c0); throw e; } finally { depth--; }
                if (r && typeof r.then === "function")
                    return r.then((v) => { timeline(name, caseName, "done", now() - c0); return v; }, (e) => { timeline(name, caseName, "threw", now() - c0); throw e; });
                timeline(name, caseName, "done", now() - c0);
                return r;
            }
            if (cfg.list) return undefined;
            if (stopped || !wanted(caseName)) {
                notSelected++;
                if (!chain || stopped) return undefined;
                depth++;
                try { return fn(); } finally { depth--; }
            }
            selected++;
            const c0 = now();
            const st = begin();
            let r;
            depth++;
            try { r = fn(); }
            catch (e) { finish(st, caseName, e === undefined ? new Error("undefined thrown") : e, c0); return undefined; }
            finally { depth--; }
            if (r && typeof r.then === "function")
                return r.then(() => finish(st, caseName, undefined, c0),
                    (e) => finish(st, caseName, e === undefined ? new Error("undefined rejection") : e, c0));
            finish(st, caseName, undefined, c0);
            return undefined;
        },
        end() {
            if (plain) return;
            if (cfg.list) {
                out.log("");
                for (const c of seen) out.log(c);
                out.log(name + ": " + seen.size + " cases listed");
                return;
            }
            if (cfg.filters.length && selected === 0 && cfg.noneOk)
                console.log(name + ": none of its " + seen.size + " cases selected");
            else if (cfg.filters.length && selected === 0) {
                console.log("FAIL: " + name + ": --case " + cfg.filters.map((f) => f.src).join(" ") + " selects none of its " + seen.size + " cases (--list prints them)");
                failed++;
            }
            if (cfg.json)
                console.log("KIT " + JSON.stringify({ suite: name, passed, failed, skipped: 0, notSelected, checks: passed + failed, failedCases: failedNames }));
            console.log(name + ": " + passed + " passed, " + failed + " failed"
                + (notSelected ? ", " + notSelected + " not selected" : "") + " (" + (now() - t0).toFixed(0) + " ms)");
            if (failed) throw new Error(name + ": " + failed + " failing section" + (failed === 1 ? "" : "s") + (failedNames.length ? " (" + failedNames.join("; ") + ")" : ""));
        },
    };
}

export { deepEqual, show };
