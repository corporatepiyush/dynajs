var __T = { pass: 0, fail: 0, cur: "", fails: [] };
var __EXP = null;
var __REQ = null;
var __CTR = {};
var __DONE = false;

var __ENGINE = (typeof print === "function") ? "dynajs" : "node";
var __Object = (typeof globalThis !== "undefined" && globalThis && globalThis.Object) ? globalThis.Object : Object;
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

function assert(c, msg) {
    if (!c) throw new Error("assert" + (msg ? " (" + msg + ")" : ""));
}

function __same(a, b) {
    if (a === b) return true;
    return typeof a === "number" && typeof b === "number" && a !== a && b !== b;
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

function assert_diverge(actual, dynajsExp, nodeExp, msg) {
    if (__same(actual, dynajsExp)) { console.log("DIVERGE " + (msg || "") + " matched=dynajs"); return; }
    if (__same(actual, nodeExp)) { console.log("DIVERGE " + (msg || "") + " matched=node"); return; }
    throw new Error("assert_diverge" + (msg ? " (" + msg + ")" : "") +
        ": got " + __repr(actual) + " want(dynajs) " + __repr(dynajsExp) +
        " OR want(node) " + __repr(nodeExp));
}

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

function __A(name, fn) { test(name, fn); }

function __rawFail(name, detail) {
    __T.fail++;
    __T.fails.push(name + ": " + detail);
}

function __mustThrow(fn) {
    try { fn(); } catch (e) { return e; }
    return null;
}

function __NP() {
    var o = {};
    if (__Object.setPrototypeOf) __Object.setPrototypeOf(o, null);
    return o;
}

function __L(id  ) {
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

var __PIN_h1 = 0x811c9dc5 | 0;
var __PIN_count = 0;
var __PIN_EXP_N = 0;
var __PIN_EXP_LINES = null;
var __PIN_DYN_LINES = null;
function __PIN(s) {
    s = String(s);
    var i, c;
    for (i = 0; i < s.length; i++) {
        c = s.charCodeAt(i);
        __PIN_h1 = Math.imul(__PIN_h1 ^ c, 0x01000193) | 0;
    }
    __PIN_h1 = Math.imul(__PIN_h1 ^ 0x0a, 0x01000193) | 0;
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
        if (typeof std !== "undefined" && std && typeof std.exit === "function") std.exit(1);
        throw new Error("SUITE-FAIL " + suiteName + " fail=" + __T.fail);
    }
}

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
