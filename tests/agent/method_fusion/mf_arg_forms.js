// Every fused argument form must compute identical results.
// Window: get_field2 <atom>; <one hot arg op>; call_method  -> OP2_call_method1_*
// plus the zero-argument OP2_call_method0.
var MF = MFHarness();

function m_arg(o, i)    { var r = o.m(i); return r; }              // get_arg0 (embedded)
function m_argw(o, i, p) { var q = p; var r = o.m(i, q); return r; } // 2 args: no fold
function m_loc(o, i)    { var j = i; var r = o.m(j); return r; }   // get_loc0 (embedded)
function m_locw(o, i)   { var j = 300; var r = o.m(j); return r; } // wide get_loc (idx >= 4 not possible for 1 local; keep wide-form coverage via many locals)
function m_chk(o, i)    { let j = i; var r = o.m(j); return r; }   // get_loc_check (TDZ-checked)
function m_push0(s)     { var r = s.charCodeAt(0); return r; }
function m_push3(s)     { var r = s.charCodeAt(3); return r; }
function m_push7(s)     { var r = s.charCodeAt(7); return r; }
function m_pushm1(s, t) { var r = s.lastIndexOf(t, -1); return r; }
function m_pushi8(s)    { var r = s.charCodeAt(100); return r; }
function m_pushi16(s)   { var r = s.charCodeAt(1000); return r; }
function m_c8(s)        { var r = s.startsWith("x"); return r; }
function m_cbig(s)      { var r = s.startsWith("yyyyyyyyyyyyyyyyyyyy"); return r; }
function m_none(o)      { var r = o.m(); return r; }

var ident = { m: function (x) { return x; } };
var half = { m: function (x) { return x / 2; } };
var TWO = "abcdefTWO";

MF.eq("get_arg", m_arg(ident, 41), 41);
MF.eq("get_loc", m_loc(ident, 42), 42);
MF.eq("get_loc_check", m_chk(ident, 43), 43);
MF.near("push_0", m_push0("abc"), 97);
MF.near("push_3", m_push3(TWO), 100); // "abcdefTWO"[3] = 'd'
MF.near("push_7", m_push7("abcdefgh"), 104);
MF.eq("push_minus1", m_pushm1("abcdef", "q"), -1);
MF.near("push_i8", m_pushi8("abcdef"), NaN);
MF.near("push_i16", m_pushi16("abcdef"), NaN);
MF.eq("push_const8", m_c8("xabc"), true);
MF.eq("push_const8-miss", m_c8("abc"), false);
MF.eq("push_const", m_cbig("yyyyyyyyyyyyyyyyyyyy!"), true);
MF.eq("method0", m_none({ m: function () { return "z"; } }), "z");
MF.eq("2args-generic", m_argw({ m: function (a, b) { return a + b; } }, 5, 6), 11);

// many-locals wide get_loc coverage (forces idx >= 4)
function m_many(o) {
  var a1=1,a2=1,a3=1,a4=1,a5=9,a6=1,a7=1,a8=1,a9=1,a10=1;
  var r = o.m(a5);
  return r * (a1+a2+a3+a4+a6+a7+a8+a9+a10);
}
MF.eq("wide-get-loc", m_many({ m: function (x) { return x * 10; } }), 810); // 90 * 9

// locals: call inside a loop body (fold inside back-edge target block)
function looped(o, n) {
  var s = 0;
  for (var i = 0; i < n; i++) s += o.m(i);
  return s;
}
MF.eq("loop-loc", looped({ m: function (x) { return x * 2; } }, 5), 20);

// string kernel shapes
function sumCharCodeAt(s, n) {
  var h = 0;
  for (var i = 0; i < n; i++) h += s.charCodeAt(i);
  return h;
}
MF.eq("charCodeAt-sum", sumCharCodeAt("hello world", 11), 1116);

function sqrtLoop(n) {
  var acc = 0;
  for (var i = 1; i <= n; i++) acc += Math.sqrt(i);
  return Math.round(acc * 100);
}
MF.eq("Math.sqrt-loop", sqrtLoop(10), 2247);

function idxOfLoop(s, n) {
  var c = 0;
  for (var i = 0; i < n; i++) if (s.indexOf("b", 1) === 1) c++;
  return c;
}
MF.eq("indexOf-loop", idxOfLoop("abcabc", 4), 4);

MF.sum("mf_arg_forms");
