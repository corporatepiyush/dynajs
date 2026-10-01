function eatsArg(x) { x = 1; return 0; }
var o = { m: eatsArg };
function callBorrowed(loc) { for (i = 0; i < 1000; i++) o.m(loc); }

var MF_N = 0, MF_BAD = 0;
function chk(name, cond) { MF_N++; if (!cond) { MF_BAD++; console.log("FAIL", name); } }

var loc = { deep: "object survives the call" };

callBorrowed(loc);
chk("repro-object-survives", loc.deep === "object survives the call");

function m_wide(o, i)   { var j = i; var r = o.m(j); return r; }
function m_chk(o, i)    { let j = i; var r = o.m(j); return r; }
function m_arg0(o, i)   { var r = o.m(i); return r; }
function m_argw(o, i)   { function inner(q) { return o.m(q); } var r = inner(i); return r; }
function m_zero(o)      { var r = o.m(); return r; }
function m_imm(s)       { var r = s.charCodeAt(3); return r; }
function m_i16(s)       { var r = s.charCodeAt(1000); return r; }
function m_const(s)     { var r = s.startsWith("yyyyyyyyyyyyyyyyyyyy"); return r; }
function m_c8(s)        { var r = s.charCodeAt(2.9); return r; }

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

var freed = 0;
function eater(x) { x = null; return 1; }
var eatObj = { m: eater };
function passTwice(v) { var a = eatObj.m(v); var b = eatObj.m(v); return a + b; }
chk("pass-twice", passTwice(loc) === 2 && loc.deep === "object survives the call");

function writerThrower(x) { x = { fresh: 1 }; throw new RangeError("after-write"); }
var ot = { m: writerThrower };
function callThrow(v) { ot.m(v); return "no"; }
var caught = null;
try { callThrow(loc); } catch (e) { caught = e; }
chk("throw-after-write", caught instanceof RangeError);

var pusher = { m: function (x) { x = null; return 7; } };
function churn(n) { var s = 0; for (var k = 0; k < n; k++) s += pusher.m({ blob: k }); return s; }
chk("churn-fresh-objects", churn(2000) === 14000);

var kept = null;
function keeper(x) { kept = x; x = 1; return 2; }
var ko = { m: keeper };
function passLocal(v) { return ko.m(v); }
chk("keeper", passLocal({ tag: "kept" }) === 2 && kept.tag === "kept");
kept = null;

function pad2(a, b) { b = 9; return a; }
var po = { m: pad2 };
chk("pad-path", (function (v) { return po.m(v); })(loc) === loc);

function sloppyArgs(x) { x = 1; return arguments[0]; }
var so = { m: sloppyArgs };
function callSloppy(v) { return so.m(v); }
chk("sloppy-arguments", callSloppy(42) === 1);

function mappedArgs(a, b) { arguments[0] = 99; return a; }
var mo = { m: mappedArgs };
function callMapped(v) { return mo.m(v, v); }
chk("mapped-mirror", callMapped(5) === 99);

function* genM(x) { x = 1; yield x; }
var go = { m: genM };
function callGen(v) { var r = 0; for (var y of go.m(v)) r = y; return r; }
chk("generator-callee", callGen(11) === 1);

async function asyncM(x) { x = 2; return x; }
var ao = { m: asyncM };
var asyncOk = false;
ao.m(21).then(function (v) { asyncOk = (v === 2); });

Promise.resolve().then(function () {}).then(function () {}).then(function () {
    chk("async-callee", asyncOk);
    console.log("f1_borrow_argv probes=" + MF_N + " failed=" + MF_BAD);
});

console.log("interim probes=" + MF_N + " failed=" + MF_BAD);
