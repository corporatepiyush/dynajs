// rv04_native_fff1 — the EXACT hazardous arity shape from the round-2 review:
// a native C function with cproto JS_CFUNC_f_f_f (dispatcher reads argv[1]
// unconditionally) and declared length 1 (argc == length skips the arity-pad
// alloca in js_call_c_function), called through a FUSED method call.
// Before the mf_argv1[2] pad fix this was a stack-buffer-overflow READ of 8
// bytes naming mf_argv1 under ASan; with the pad it must run clean and yield
// NaN (ToNumber(undefined) in the unused second slot).
//
// Gate: ASan-clean on the patched build; output byte-matches itself across
// binaries (the value NaN is engine-independent here).
import { fff1 } from "./fff1.so";

var N = 0, BAD = 0;
function chk(name, cond) { N++; if (!cond) { BAD++; console.log("FAIL", name); } }

var mod = { f: fff1 };

// fused method1_loc: [get_field2 f][get_loc x][call_method 1] -> fallback
// js_call_c_function with argc == 1 == declared length
function callLoc(m, x) {
    var r = m.f(x);
    return r;
}
chk("fff1-loc-nan", isNaN(callLoc(mod, 3)));

// fused method1_imm8 immediate arg, same path. NOTE: written NON-tail
// (`var r = ...; return r;`) deliberately — `return m.f(2);` compiles to the
// generic OP_tail_call_method (argc=1), which has a PRE-EXISTING engine
// hazard with f_f_f length-1 callees (arg_buf[1] read past the interpreter
// stack top; ASan dynamic-stack-buffer-overflow, reproduces on pristine —
// out of this lane's scope). The fused path is what this lane owns.
function callImm(m) {
    var r = m.f(2);
    return r;
}
chk("fff1-imm-nan", isNaN(callImm(mod)));

// hot loop: repeated fused calls into the shape (ASan watches every read)
var nanCount = 0;
for (var i = 0; i < 500; i++) {
    var v = callLoc(mod, i);
    if (isNaN(v)) nanCount++;
}
chk("fff1-loop", nanCount === 500);

// full-arity call (control: argc == 2 passes both real args)
function callTwo(m, a, b) {
    return m.ff(a, b);
}
var mod2 = { ff: fff1 };
chk("fff1-two-args", callTwo(mod2, 2, 3) === 5);

console.log("rv04_native_fff1 probes=" + N + " failed=" + BAD);
