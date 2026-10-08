// tests/kit.js -- the suite kit: named cases, a case filter, assertions that
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
// Arguments after the file name (also: ./build.sh t FILE --case=PAT):
//   --case=PAT   run only the cases whose name contains PAT; /re/flags is a
//                regular expression. Repeatable; a case runs if ANY matches.
//                A filter that selects nothing is a failure, never a pass.
//   --list       print the case names and run nothing
//   --bail       stop at the first failing case
//   --verbose    one line per case, with its check count and time
//   --json       end with one machine-readable line: KIT {...}
//
// A case FAILS when an assertion fails, when it throws, or when it outlives
// its timeoutMs. A failed assertion does not abort the case (the rest of it
// still reports), except a.must(), which does. a.skip(why) ends the case as a
// SKIP that is printed and counted: a skipped case is never a silent pass.
//
// The last line is "<suite>: P passed, F failed[, S skipped]"; any failure also
// throws, so the process exits non-zero. Lines starting with FAIL are what the
// parallel runner greps for.

const ARGV = (globalThis.scriptArgs || []).slice(1);
const now = () => (typeof performance === "object" ? performance.now() : Date.now());

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
    const o = { filters: [], list: false, bail: false, verbose: false, json: false };
    for (let i = 0; i < argv.length; i++) {
        const s = String(argv[i]);
        let pat = null;
        if (s === "--list") o.list = true;
        else if (s === "--bail") o.bail = true;
        else if (s === "--verbose") o.verbose = true;
        else if (s === "--json") o.json = true;
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
    const cases = [], afters = [];
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
                for (let r = 0; r < (reps || 5); r++) { const t0 = now(); fn(); b = Math.min(b, now() - t0); }
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
        test(caseName, fn, o) { cases.push({ name: String(caseName), fn, opts: o || {}, skip: null }); },
        skip(caseName, why) { cases.push({ name: String(caseName), fn: null, opts: {}, skip: why || "no reason given" }); },
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
                for (const c of cases) console.log(c.name);
                console.log(name + ": " + cases.length + " cases listed");
                return;
            }
            const chosen = cases.filter((c) => wanted(c.name));
            const t0 = now();
            let passed = 0, failed = 0, skipped = 0, checks = 0;
            const failedNames = [];
            if (cfg.filters.length && chosen.length === 0) {
                console.log("FAIL: " + name + ": --case " + cfg.filters.map((f) => f.src).join(" ") + " selects none of its " + cases.length + " cases (--list prints them)");
                failed = 1;
            }
            for (const c of chosen) {
                c.passed = 0; c.failed = 0; c.msgs = [];
                const c0 = now();
                let state = "ok", why = "";
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
                    if (state === "ok" && c.passed === 0) {
                        state = "fail"; c.failed++;
                        console.log("FAIL: [" + c.name + "] made no assertion (a case that checks nothing cannot pass)");
                    }
                }
                const ms = now() - c0;
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
            const line = name + ": " + passed + " passed, " + failed + " failed"
                + (skipped ? ", " + skipped + " SKIPPED" : "")
                + (left ? ", " + left + " not selected" : "")
                + " (" + checks + " checks, " + (now() - t0).toFixed(0) + " ms)";
            if (cfg.json)
                console.log("KIT " + JSON.stringify({ suite: name, passed, failed, skipped, notSelected: left, checks, failedCases: failedNames }));
            console.log(line);
            if (failed) throw new Error(name + ": " + failed + " failing case" + (failed === 1 ? "" : "s") + (failedNames.length ? " (" + failedNames.join("; ") + ")" : ""));
        },
    };
    return api;
}

export { deepEqual, show };
