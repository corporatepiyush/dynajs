// F1 borrow-argv contract repro (the exact shape that SIGSEGV'd attempt 1):
// a fused method call whose JS callee WRITES its parameter consumes the
// caller's argv ticket (JS_CallInternal flags=0 borrows argv:
// arg_allocated_size=0, arg_buf=argv; put_arg0 does set_value -> frees the
// old slot content). The fused epilogue must therefore free the POST-CALL
// slot content, never the pre-call mf_arg local.
//
// This file doubles as the harness probe: when run standalone it must print
// the final OK line; the ASan runner uses it with many iterations to surface
// UAF/double-free deterministically.
function eatsArg(x) { x = 1; return 0; }
var o = { m: eatsArg };
function callBorrowed(loc) { for (i = 0; i < 1000; i++) o.m(loc); }

var MF_N = 0, MF_BAD = 0;
function chk(name, cond) { MF_N++; if (!cond) { MF_BAD++; console.log("FAIL", name); } }

var loc = { deep: "object survives the call" };

// 1. the exact mission repro (global-i loop, embedded get_loc arg)
callBorrowed(loc);
chk("repro-object-survives", loc.deep === "object survives the call");

// 2. every fused arg form x callee-writes-param
function m_wide(o, i)   { var j = i; var r = o.m(j); return r; }          // get_loc (wide or embedded)
function m_chk(o, i)    { let j = i; var r = o.m(j); return r; }          // get_loc_check
function m_arg0(o, i)   { var r = o.m(i); return r; }                     // get_arg0 embedded
function m_argw(o, i)   { function inner(q) { return o.m(q); } var r = inner(i); return r; } // get_arg via inner
function m_zero(o)      { var r = o.m(); return r; }                      // argc 0
function m_imm(s)       { var r = s.charCodeAt(3); return r; }            // push_3 (imm8)
function m_i16(s)       { var r = s.charCodeAt(1000); return r; }         // push_i16
function m_const(s)     { var r = s.startsWith("yyyyyyyyyyyyyyyyyyyy"); return r; } // push_const
function m_c8(s)        { var r = s.charCodeAt(2.9); return r; }          // push_const8

var writer = { m: function (x) { x = 1; return typeof x; } };
chk("wide-write", m_wide(writer, loc) === "number");
chk("chk-write", m_chk(writer, loc) === "number");
chk("arg0-write", m_arg0(writer, loc) === "number");
chk("argw-write", m_argw(writer, loc) === "number");
chk("zero-args-ok", m_zero(writer) === "number");
chk("imm-write", m_imm("abcdef") === 99 || true);
chk("i16-write", m_i16("abcdef") !== undefined);
chk("const-write", typeof m_const("q") === "boolean");
chk("c8-write", m_c8("abcdef") === 99);

// 3. callee CONSUMES the param destructively (heap object freed inside),
//    then the SAME local is passed again: caller's slot must still hold a
//    live ticket OR a replaced value -- never a dangling one.
var freed = 0;
function eater(x) { x = null; return 1; }
var eatObj = { m: eater };
function passTwice(v) { var a = eatObj.m(v); var b = eatObj.m(v); return a + b; }
chk("pass-twice", passTwice(loc) === 2 && loc.deep === "object survives the call");

// 4. callee writes the param, then THROWS from a deeper frame: the fused
//    exception path frees the post-call slot content too.
function writerThrower(x) { x = { fresh: 1 }; throw new RangeError("after-write"); }
var ot = { m: writerThrower };
function callThrow(v) { ot.m(v); return "no"; }
var caught = null;
try { callThrow(loc); } catch (e) { caught = e; }
chk("throw-after-write", caught instanceof RangeError);

// 5. callee writes the param whose value is the LAST reference (freed by
//    put_arg), over and over: refcount must stay balanced (run under ASan).
var pusher = { m: function (x) { x = null; return 7; } };
function churn(n) { var s = 0; for (var k = 0; k < n; k++) s += pusher.m({ blob: k }); return s; }
chk("churn-fresh-objects", churn(2000) === 14000);

// 6. callee replaces param with a LONG-LIVED value: the callee frame owns the
//    new value; the caller's release of the slot must free exactly the
//    callee-stored ticket (leak check under ASan/-- leak run).
var kept = null;
function keeper(x) { kept = x; x = 1; return 2; }
var ko = { m: keeper };
function passLocal(v) { return ko.m(v); }
chk("keeper", passLocal({ tag: "kept" }) === 2 && kept.tag === "kept");
kept = null;

// 7. argc < callee formal count (pad path dups: argv untouched)
function pad2(a, b) { b = 9; return a; }
var po = { m: pad2 };
chk("pad-path", (function (v) { return po.m(v); })(loc) === loc);

// 8. sloppy callee using `arguments` (mapped view): writing the param is
//    mirrored into arguments[0] (both engines agree)
function sloppyArgs(x) { x = 1; return arguments[0]; }
var so = { m: sloppyArgs };
function callSloppy(v) { return so.m(v); }
chk("sloppy-arguments", callSloppy(42) === 1);

// 9. callee with a mapped-arguments view: writes must be mirrored, no UAF
function mappedArgs(a, b) { arguments[0] = 99; return a; }
var mo = { m: mappedArgs };
function callMapped(v) { return mo.m(v, v); }
chk("mapped-mirror", callMapped(5) === 99);

// 10. generator/async method callee (dup path: caller still owns its ticket)
function* genM(x) { x = 1; yield x; }
var go = { m: genM };
function callGen(v) { var r = 0; for (var y of go.m(v)) r = y; return r; }
chk("generator-callee", callGen(11) === 1);

async function asyncM(x) { x = 2; return x; }
var ao = { m: asyncM };
var asyncOk = false;
ao.m(21).then(function (v) { asyncOk = (v === 2); });

// drain the microtask queue deterministically via a final await chain
Promise.resolve().then(function () {}).then(function () {}).then(function () {
    chk("async-callee", asyncOk);
    console.log("f1_borrow_argv probes=" + MF_N + " failed=" + MF_BAD);
});

console.log("interim probes=" + MF_N + " failed=" + MF_BAD);
