/* tests/agent/_h/h.js — shared portable assert harness for the converted
 * .agent test suites. ZERO imports: runs unmodified on dynajs and node.
 *
 * Converted probes are plain scripts that:
 *   - assign their baked expectations to __EXP (per-call-site oracle lines),
 *   - replace former console.log-oracle calls with __A()/__L() calls,
 *   - call summary("<suite>") (sync) or __FINISH("<suite>") (async) at the end.
 *
 * summary() prints "SUITE <name> PASS <p> FAIL <f>" and, when FAIL > 0, one
 * "FAIL <test>" line per failure, then forces a nonzero exit (std.exit when
 * available, uncaught throw otherwise; the run.sh gate treats FAIL>0 as
 * failure regardless, covering engines where an async throw cannot move the
 * exit code).
 */

var __T = { pass: 0, fail: 0, cur: "", fails: [] };
var __EXP = null;  /* id -> [expected line, ...] (baked from the node oracle) */
var __REQ = null;  /* optional {dynajs:{id:requiredCalls}, node:{id:requiredCalls}} */
var __CTR = {};    /* id -> times the call site executed */
var __DONE = false;

/* engine detection (conservative): dynajs exposes the print() global, node
 * does not. Used only to pick which expectation set is REQUIRED so that
 * engine-divergent control flow (one engine takes a branch the other never
 * runs) is not reported as unreached on the engine that never had it. */
var __ENGINE = (typeof print === "function") ? "dynajs" : "node";
/* probes under test may rebind globals (e.g. `var Object = 42`); harness
 * code must use the pristine references captured below */
var __Object = (typeof globalThis !== "undefined" && globalThis && globalThis.Object) ? globalThis.Object : Object;
/* NOTE: __Object is read via the globalThis PROPERTY, not the bare `Object`
 * binding: a probe declaring `var Object = 42` hoists a shadowing binding
 * over the whole concatenated script, and only the globalThis property still
 * reaches the real constructor.
 * Also: no helper shims with common short names (eq/ok/deepEq) here — suites
 * that need them prepend their own helper file (run.sh concatenates
 * <suite>/_h.js when present); short-name globals here would collide with
 * probe locals like `let ok = 0`. */

/* ---------- portable value formatting (identical on both engines) ------- */

function __isNegZero(v) { return v === 0 && 1 / v === -1 / 0; }

function __repr(v) {
    var t = typeof v;
    if (t === "string") return JSON.stringify(v);
    if (t === "number") {
        if (v !== v) return "NaN";
        if (v === 1 / 0) return "Infinity";
        if (v === -1 / 0) return "-Infinity";
        if (__isNegZero(v)) return "-0";
        return String(v);
    }
    if (t === "bigint") return String(v) + "n";
    if (t === "undefined") return "undefined";
    if (t === "boolean") return String(v);
    if (t === "function") return "function " + (v.name ? v.name : "(anonymous)");
    if (t === "symbol") return v.toString();
    if (v === null) return "null";
    try {
        return JSON.stringify(v, function (k, x) {
            if (typeof x === "bigint") return String(x);
            if (typeof x === "function" || typeof x === "symbol") return String(x);
            return x;
        });
    } catch (e) {
        try { return String(v); } catch (e2) { return "[unprintable]"; }
    }
}

/* tagged value: [tag, scalarString, reprString]
 *   tag: str num big bool und nul sym obj fun
 * Used by both the converted probes (via __fmt1) and the converter's capture
 * shim, so baked expectations and runtime formatting can never drift. */
function __tagval(v) {
    var t = typeof v;
    if (t === "string") return ["str", v, JSON.stringify(v)];
    if (t === "number") return ["num", "", __repr(v)];
    if (t === "bigint") return ["big", String(v), String(v) + "n"];
    if (t === "boolean") return ["bool", v ? "true" : "false", v ? "true" : "false"];
    if (t === "undefined") return ["und", "", "undefined"];
    if (v === null) return ["nul", "", "null"];
    if (t === "function") return ["fun", "", __repr(v)];
    if (t === "symbol") return ["sym", "", __repr(v)];
    return ["obj", "", __repr(v)];
}

/* format one console.log-style argument the way the capture pass did:
 * strings print raw, bigints print without the "n" suffix (matching
 * console.log on both engines), everything else uses __repr. */
function __fmt1(v) {
    var tv = __tagval(v);
    if (tv[0] === "str") return tv[1];
    if (tv[0] === "big") return tv[1];
    return tv[2];
}

function __fmtLine(args) {
    var n = (args.n !== undefined) ? args.n : args.length;
    var s = "", i;
    for (i = 0; i < n; i++) {
        if (i > 0) s += " ";
        s += __fmt1(args[i]);
    }
    return s;
}

/* ---------- core asserts (throwing; use inside test/__A) ---------------- */

function assert(c, msg) {
    if (!c) throw new Error("assert" + (msg ? " (" + msg + ")" : ""));
}

function __same(a, b) {
    if (a === b) return true;
    return typeof a === "number" && typeof b === "number" && a !== a && b !== b; /* NaN */
}

function assert_eq(a, b, msg) {
    if (!__same(a, b)) {
        throw new Error("assert_eq" + (msg ? " (" + msg + ")" : "") +
            ": got " + __repr(a) + " want " + __repr(b));
    }
}

function assert_ne(a, b, msg) {
    if (__same(a, b)) {
        throw new Error("assert_ne" + (msg ? " (" + msg + ")" : "") +
            ": both " + __repr(a));
    }
}

function assert_throws(fn, ctorName) {
    var threw = false, e;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("assert_throws" + (ctorName ? " " + ctorName : "") +
        ": no throw" + (__T.cur ? " [" + __T.cur + "]" : ""));
    if (ctorName === undefined || ctorName === null) return;
    var got = (e && e.constructor && e.constructor.name) ? e.constructor.name : typeof e;
    if (got !== ctorName) {
        throw new Error("assert_throws " + ctorName + ": got " + got +
            " (" + (e && e.message ? e.message : String(e)) + ")");
    }
}

function assert_contains(s, sub, msg) {
    if (String(s).indexOf(sub) < 0) {
        throw new Error("assert_contains" + (msg ? " (" + msg + ")" : "") +
            ": " + __repr(String(s)) + " does not contain " + __repr(sub));
    }
}

/* explicit engine-divergence marker: passes if actual matches EITHER baked
 * expectation and prints a DIVERGE line recording which one matched (kept in
 * the per-probe .txt the runner writes; never a silent skip). */
function assert_diverge(actual, dynajsExp, nodeExp, msg) {
    if (__same(actual, dynajsExp)) { console.log("DIVERGE " + (msg || "") + " matched=dynajs"); return; }
    if (__same(actual, nodeExp)) { console.log("DIVERGE " + (msg || "") + " matched=node"); return; }
    throw new Error("assert_diverge" + (msg ? " (" + msg + ")" : "") +
        ": got " + __repr(actual) + " want(dynajs) " + __repr(dynajsExp) +
        " OR want(node) " + __repr(nodeExp));
}

/* ---------- named tests -------------------------------------------------- */

function test(name, fn) {
    var save = __T.cur;
    __T.cur = name;
    try { fn(); __T.pass++; }
    catch (e) {
        __T.fail++;
        __T.fails.push(name + ": " + (e && e.message ? e.message : String(e)));
    }
    __T.cur = save;
}

/* named inline assert used at the converted call site: preserves the scope
 * of the original expression while counting through the harness */
function __A(name, fn) { test(name, fn); }

/* ---------- raw counters (used by __L / transcript pins) ----------------- */

function __rawFail(name, detail) {
    __T.fail++;
    __T.fails.push(name + ": " + detail);
}

/* run fn, return the thrown error (null if none) — for throw-contract tests */
function __mustThrow(fn) {
    try { fn(); } catch (e) { return e; }
    return null;
}

/* pollution-proof plain object: some probes deliberately poison
 * Array.prototype / Object.prototype index properties, so the harness must
 * never rely on inherited array index writes (Array.push uses [[Set]] with
 * throw=true and can throw on a poisoned prototype). */
function __NP() {
    var o = {};
    if (__Object.setPrototypeOf) __Object.setPrototypeOf(o, null);
    return o;
}

/* line-oracle call site: evaluates args in place (scope preserved) and
 * checks the formatted line against the baked expectation for execution #n.
 * An expectation entry is either a string (must match exactly; a leading "~"
 * means prefix-match for run-varying lines like stack-depth counters) or a
 * [dynajsLine, nodeLine] array (engine divergence: either match passes and a
 * DIVERGE line records which engine matched; never a silent skip). */
function __L(id /*, args... */) {
    var n = arguments.length - 1, args = __NP(), i;
    for (i = 0; i < n; i++) args[i] = arguments[i + 1];
    args.n = n;
    __CTR[id] = (__CTR[id] || 0) + 1;
    var x = __CTR[id];
    var exps = (__EXP && __EXP[id]) ? __EXP[id] : [];
    var line = __fmtLine(args);
    if (x - 1 < exps.length) {
        var e = exps[x - 1];
        var matches = function (want, line) {
            if (typeof want === "string" && want.charAt(0) === "~") {
                return want.length > 1 && line.lastIndexOf(want.slice(1), 0) === 0;
            }
            return want === line;
        };
        if (typeof e === "string") {
            if (matches(e, line)) __T.pass++;
            else __rawFail("#" + id + "." + x, "got [" + line + "] want [" + e + "]");
        } else if (e && e.length === 2) {
            if (matches(e[0], line)) { console.log("DIVERGE #" + id + "." + x + " matched=dynajs"); __T.pass++; }
            else if (matches(e[1], line)) { console.log("DIVERGE #" + id + "." + x + " matched=node"); __T.pass++; }
            else __rawFail("#" + id + "." + x, "got [" + line + "] want(dynajs) [" + e[0] + "] want(node) [" + e[1] + "]");
        } else {
            __rawFail("#" + id + "." + x, "malformed expectation entry");
        }
    } else {
        __rawFail("#" + id + "." + x, "unexpected call, line [" + line + "]");
    }
}

/* transcript-pin recorder: feeds a rolling FNV-1a hash over the emitted
 * record stream, exact-checks the first __PIN_EXP_N records against the
 * baked oracle lines (records longer than 5000 chars are digest-only), and
 * counts records so the digest + count + prefix together pin the transcript */
var __PIN_h1 = 0x811c9dc5 | 0;
var __PIN_count = 0;
var __PIN_EXP_N = 0;
var __PIN_EXP_LINES = null;      /* node-oracle records (string) */
var __PIN_DYN_LINES = null;      /* optional dynajs-oracle records for divergent transcripts */
function __PIN(s) {
    s = String(s);
    var i, c;
    for (i = 0; i < s.length; i++) {
        c = s.charCodeAt(i);
        __PIN_h1 = Math.imul(__PIN_h1 ^ c, 0x01000193) | 0;
    }
    __PIN_h1 = Math.imul(__PIN_h1 ^ 0x0a, 0x01000193) | 0; /* separator, as baked */
    __PIN_count++;
    if (__PIN_count <= __PIN_EXP_N && __PIN_EXP_LINES) {
        var want = __PIN_EXP_LINES[__PIN_count - 1];
        var k = 160;
        var okNode = (s.length > 5000 || want.length > 5000)
            ? s.slice(0, k) === want.slice(0, k) : s === want;
        if (!okNode && __PIN_DYN_LINES) {
            var wantD = __PIN_DYN_LINES[__PIN_count - 1];
            var okD = (s.length > 5000 || wantD.length > 5000)
                ? s.slice(0, k) === wantD.slice(0, k) : s === wantD;
            if (okD) { console.log("DIVERGE pin-record." + __PIN_count + " matched=dynajs"); return; }
        }
        if (!okNode) {
            __rawFail("pin-record." + __PIN_count,
                "got [" + s.slice(0, 200) + "] want [" + String(want).slice(0, 200) + "]");
        }
    }
}

/* ---------- summary / finish --------------------------------------------- */

function __missingCount() {
    var id, k = 0;
    if (__REQ) {
        var req = __REQ[__ENGINE] || {};
        for (id in req) {
            var used = __CTR[id] || 0;
            if (req[id] > used) k += req[id] - used;
        }
        return k;
    }
    if (!__EXP) return 0;
    for (id in __EXP) {
        var used2 = __CTR[id] || 0;
        if (__EXP[id].length > used2) k += __EXP[id].length - used2;
    }
    return k;
}

function summary(suiteName) {
    if (__DONE) return;
    __DONE = true;
    var missing = __missingCount();
    if (missing > 0) {
        __rawFail("unreached-asserts",
            missing + " baked expectation(s) were never consumed (a converted call site did not run)");
    }
    var i;
    console.log("SUITE " + suiteName + " PASS " + __T.pass + " FAIL " + __T.fail);
    for (i = 0; i < __T.fails.length; i++) console.log("FAIL " + __T.fails[i]);
    if (__T.fail > 0) {
        /* force nonzero exit on both engines. std.exit covers dynajs --std and
         * async contexts (a timer throw does not move dynajs' exit code);
         * the uncaught throw covers node and plain dynajs (sync context). */
        if (typeof std !== "undefined" && std && typeof std.exit === "function") std.exit(1);
        throw new Error("SUITE-FAIL " + suiteName + " fail=" + __T.fail);
    }
}

/* async-aware finisher: microtask chains (await / Promise.all / async
 * generators) always drain before the first timer fires, so a short timer is
 * a portable "queue is quiet" signal; re-arm while baked expectations remain
 * unconsumed (bounded), then summarize regardless. */
function __FINISH(suiteName) {
    var tries = 0;
    function attempt() {
        if (__missingCount() > 0 && tries < 60) {
            tries++;
            setTimeout(attempt, tries <= 20 ? 10 : 50);
            return;
        }
        summary(suiteName);
    }
    setTimeout(attempt, 10);
}
