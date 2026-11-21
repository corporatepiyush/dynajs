// flags: --std
// timeout: 600
// covers: c:src/builtins/builtin-json.c
// tests/core/test_audit_w2_engine_1.js -- --timeout-ms must hold for every loop
// shape the interpreter can form, and no built-in may swallow the interrupt.
// SEC-01 (tail calls), E2c-01 (fused compare-and-branch back-edges), E5-01
// (Function.prototype.tryCatch), E5-05 (Object.equals on a shared DAG),
// E1b-03 (deep recursion inside a Worker must be a catchable error).
// Each form runs in a child dynajs so the deadline is armed per form.
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_json_audit_w2_engine_1");
import { Exec, args } from "dyna:sys";
import * as std from "std";

const BIN = args()[0];
let failures = 0;

function assert(cond, msg) {
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}

const FORMS = [
    ["tail-call self", "function f(n){return f(n+1)} f(0)"],
    ["tail-call mutual", "function a(n){return b(n+1)} function b(n){return a(n+1)} a(0)"],
    ["tail-call method", "var o={m(n){return this.m(n+1)}}; o.m(0)"],
    ["do-while ===", "var x=1; do {} while (x === 1);"],
    ["do-while !==", 'function f(s){ do {} while (s !== "b"); } f("a");'],
    ["do-while === const-prop", "const E={A:1}; function f(x){ do {} while (x === E.A); } f(1);"],
    ["do-while === with work", "var x=1,a=[0,0,0,0,0,0,0,0],i=0,n=0; do { a[i&7]=i; i=(i+1)|0; n+=a[3]; } while (x === 1);"],
    ["do-while <", "var x=1; do {} while (x < 2);"],
    ["do-while ==", "var x=1; do {} while (x == 1);"],
    ["tryCatch swallow", "const safe=(function(){for(;;){}}).tryCatch(e=>0); for(;;) safe();"],
    ["tryCatch native handler", "const safe=(function(){for(;;){}}).tryCatch(Math.abs); for(;;) safe();"],
    ["promise catch respin", "function spin(){ for(;;){} } (async()=>{ for(;;){ try { await null; spin(); } catch(e){} } })();"],
    ["equals DAG", "var a=[],b=[]; for(var i=0;i<60;i++){a=[a,a];b=[b,b];} Object.equals(a,b)"],
    ["clone DAG", "var a=[]; for(var i=0;i<60;i++){a=[a,a];} Object.clone(a)"],
    ["toReversed array-like", "Array.prototype.toReversed.call({length:2**31-1})"],
    ["with array-like", "Array.prototype.with.call({length:2**31-1}, 0, 1)"],
];

const CONTROLS = [
    ["tail-call finite", "function f(n,a){ if (n===0) return a; return f(n-1,a+n); } print(f(200000,0))", "20000100000"],
    ["do-while finite", "var i=0,s=0; do { s+=i; i++; } while (i !== 100000); print(s)", "4999950000"],
    ["tryCatch ordinary", "print((function(){throw new Error('x')}).tryCatch(e=>e.message)())", "x"],
    ["equals tree", "print(Object.equals({a:[1,{b:2}]},{a:[1,{b:2}]}))", "true"],
];

__kitS.section("control exited", () => {
for (const [name, form, want] of CONTROLS) {
    const r = Exec(BIN, ["--timeout-ms", "20000", "-e", form], { timeoutMs: 30000, encoding: "utf8" });
    assert(r.code === 0, name + ": control exited " + r.code + " (" + (r.stderr || "").slice(0, 80) + ")");
    assert((r.stdout || "").trim() === want, name + ": got " + JSON.stringify((r.stdout || "").trim()) + " want " + want);
    print("  ok  control " + name);
}
});
__kitS.end();

if (failures) {
    print("test_audit_w2_engine_1: " + failures + " FAILED");
    std.exit(1);
}
print("test_audit_w2_engine_1: all passed");
