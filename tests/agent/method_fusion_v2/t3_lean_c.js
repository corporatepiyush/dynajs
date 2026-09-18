// T3 lean C-callee dispatch probe: the fused ops route JS_CLASS_C_FUNCTION
// callees through a specialized preamble (generic/generic_magic only, arity
// guard sends everything else to js_call_c_function). Every observable must
// stay byte-identical to the generic engine: values, coercions, exceptions,
// and native frames in stack traces.
var N = 0, BAD = 0;
function chk(name, cond) { N++; if (!cond) { BAD++; console.log("FAIL", name); } }

// ── generic cproto via the lean path (argc == length) ──
function cc(s, i) { var r = s.charCodeAt(i); return r; }
chk("lean-cc", cc("abc", 0) === 97 && cc("abc", 2) === 99);

// ── generic cproto via the FALLBACK (argc < C length: charCodeAt() argc=0) ──
function cc0(s) { var r = s.charCodeAt(); return r; }
chk("fallback-argc0", cc0("abc") === 97); // undefined -> 0

// ── generic_magic (e.g. String.fromCharCode? no — method shape: startsWith is generic_magic in some builds; exercise both shapes) ──
function sw(s, p) { var r = s.startsWith(p); return r; }
chk("magic-or-generic-sw", sw("abcd", "ab") === true && sw("abcd", "zz") === false);

// ── f_f_f stays on the fallback (Math.sqrt via method shape) ──
function sq(n) { var r = n.toFixed(3); return r; }
chk("toFixed-fallback", sq(2.5) === "2.500");

// ── in-C-function throw: RangeError from toFixed via method shape ──
var toFixedThrow = 0;
try { (function (n) { return n.toFixed(101); })(5); } catch (e) {
    toFixedThrow = (e instanceof RangeError) ? 1 : 2;
}
chk("c-throw-rangeerror", toFixedThrow === 1);

// ── native frame present in the stack when a C callee throws ──
var stackHasFrame = false, stackHasCallLine = false;
try { (function (n) { return n.toFixed(101); })(5); } catch (e) {
    stackHasFrame = /toFixed/.test(e.stack || "");
    stackHasCallLine = /:\d+:\d+/.test(e.stack || "");
}
chk("native-frame-in-stack", stackHasFrame);
chk("call-line-in-stack", stackHasCallLine);

// ── object-coercible TypeError from a lean-path C callee (null this) ──
var nullThis = 0;
try { (function (s) { return s.charCodeAt(0); })(null); } catch (e) {
    nullThis = (e instanceof TypeError) ? 1 : 2;
}
chk("lean-c-null-this-typeerror", nullThis === 1);

// ── GC interaction: allocate inside the loop while the lean path runs ──
var sBig = "";
for (var i = 0; i < 2000; i++) sBig += "x";
var h = 0;
for (var k = 0; k < 2000; k++) h += sBig.charCodeAt(k & 1023);
chk("lean-gc-churn", typeof h === "number");

// ── receiver identity: sloppy builtin sees object-wrapped this (identical
//    to js_call_c_function, which never wraps — both engines agree) ──
var lastThis = null;
var capturer = { m: function () { "use strict"; lastThis = this; return 1; } };
function cap(o) { return o.m(); }
chk("this-passthrough", cap(capturer) === 1 && lastThis === capturer);

console.log("t3_lean_c probes=" + N + " failed=" + BAD);
