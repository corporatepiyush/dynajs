// rv06_preexisting_tailfff1_repro - REPRO FOR A PRE-EXISTING ENGINE BUG (not
// this lane's defect; found while building the rv04 regression). `return m.f(2);`
// compiles to generic OP_tail_call_method with argc=1; the callee here is a
// native JS_CFUNC_f_f_f with declared length 1 (built by build_fff1.sh), so
// js_call_c_function skips the arity pad and its f_f_f case reads arg_buf[1]
// == sp[0] - one past the interpreter frame's stack region. ASan (pristine
// AND patched, identical): dynamic-stack-buffer-overflow READ of 8 at
// property_set_convert.inc.c:8728 in js_call_c_function. Functionally silent
// in release builds (the garbage slot converts to NaN). Candidate fix
// (separate lane): guard the f_f/f_f_f d2 reads with argc, or pad argv in
// OP_tail_call/OP_tail_call_method.
import { fff1 } from "./fff1.so";
var mod = { f: fff1 };
function callImm(m) {
    return m.f(2);
}
console.log(callImm(mod));
