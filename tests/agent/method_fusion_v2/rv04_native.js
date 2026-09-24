// rv04_native — permanent regression for the fused-call argv arity contract.
//
// Shape: a FUSED method call (get_field2 <atom>; <one hot arg op>;
// call_method) whose loaded callee is a C function with cproto f_f (enum
// JS_CFUNC_f_f_f) and declared length 1, invoked with argc == 1 == length.
// js_call_c_function then skips the arity-pad alloca (argc >= arg_count) and
// its f_f_f case reads arg_buf[1] PAST the argument. The unfused stream
// carries the interpreter-stack func slot there; the fused op must provide a
// defined pad slot (mf_argv1[1] = JS_UNDEFINED) or this is a stack OOB read
// (ASan: stack-buffer-overflow READ of 8, naming mf_argv1).
//
// Gate: this file must run ASan-clean and byte-match the node oracle.
var N = 0, BAD = 0;
function chk(name, cond) { N++; if (!cond) { BAD++; console.log("FAIL", name); } }

var M = Math;

// fused method1_loc (local arg) into f_f/f_f_f length-1 builtins
function absLoc(x)  { var r = M.abs(x);  return r; }
function floorLoc(x){ var r = M.floor(x); return r; }
function ceilLoc(x) { var r = M.ceil(x);  return r; }
function roundLoc(x){ var r = M.round(x); return r; }
function sqrtLoc(x) { var r = M.sqrt(x);  return r; }
function expLoc(x)  { var r = M.exp(x);   return r; }

chk("abs-loc", absLoc(-4.2) === 4.2);
chk("floor-loc", floorLoc(3.9) === 3);
chk("ceil-loc", ceilLoc(3.1) === 4);
chk("round-loc", roundLoc(3.5) === 4);
chk("sqrt-loc", sqrtLoc(9) === 3);
chk("exp-loc", Math.round(expLoc(2) * 1e6) === 7389056);

// fused method1_imm8 (immediate arg) — same fallback path, imm form
function absImm()  { return M.abs(-3); }
function sqrtImm() { return M.sqrt(4); }
chk("abs-imm8", absImm() === 3);
chk("sqrt-imm8", sqrtImm() === 2);

// fused method1_loc in a hot loop (the kernel shape; ASan watches every call)
var s = 0;
for (var i = 1; i <= 500; i++) s += M.sqrt(i);
chk("sqrt-loop", Math.round(s * 1000) === 7464534);

// argc 0 < length: the pad-alloca path (control — must stay correct too)
function sqrt0() { return M.sqrt(); }
chk("sqrt-argc0", isNaN(sqrt0()));

// NaN propagation through the pad slot (slot[1] = JS_UNDEFINED -> ToFloat64
// of the unused second arg is NaN; unary f ignores it — value must equal the
// unfused engine's result, which reads a garbage-but-NaN-converting slot)
chk("sqrt-nan-arg", isNaN(M.sqrt(-1)));
chk("abs-nan-arg", isNaN(M.abs(undefined)));

// getter-loaded native callee (load slow path, same call machinery)
var holder = {};
Object.defineProperty(holder, "m", {
    get: function () { return M.sqrt; },
    configurable: true
});
function getterSqrt(o, v) { return o.m(v); }
chk("getter-native", getterSqrt(holder, 16) === 4);

// proto-walk to a native callee
var protoHost = Object.create(M);
function protoSqrt(o, v) { return o.sqrt(v); }
chk("proto-native", protoSqrt(protoHost, 25) === 5);

// callee-writes-param is a JS-callee shape — include it here so the ASan run
// of this file covers both argv contracts in one sweep
var eater = { m: function (x) { x = 1; return x; } };
function eat(o, v) { return o.m(v); }
chk("eat-param", eat(eater, 41) === 1);

console.log("rv04_native probes=" + N + " failed=" + BAD);
