// flags: --std
// timeout: 600
// tests/test_audit_w2_engine_1.js -- --timeout-ms must hold for every loop
// shape the interpreter can form, and no built-in may swallow the interrupt.
// SEC-01 (tail calls), E2c-01 (fused compare-and-branch back-edges), E5-01
// (Function.prototype.tryCatch), E5-05 (Object.equals on a shared DAG),
// E1b-03 (deep recursion inside a Worker must be a catchable error).
// Each form runs in a child dynajs so the deadline is armed per form.
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
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

for (const [name, form] of FORMS) {
    const t0 = performance.now();
    const r = Exec(BIN, ["--timeout-ms", "300", "-e", form], {
        timeoutMs: 20000,
        encoding: "utf8",
    });
    const ms = performance.now() - t0;
    const out = r.stdout + r.stderr;
    assert(r.code !== 0 || r.signal !== null,
        name + ": child exited 0 with no interrupt (out=" + out.slice(0, 80) + ")");
    assert(/interrupt|out of memory|invalid array length/i.test(out),
        name + ": no interrupted error (out=" + out.slice(0, 120) + ")");
    assert(!r.timedOut && ms < 10000,
        name + ": took " + (ms | 0) + "ms, deadline not enforced promptly");
    print("  ok  " + name + " stopped in " + (ms | 0) + "ms");
}

// Controls: the same shapes must still terminate normally and give the right
// value when they are NOT infinite -- a poll that fires on every edge, or a
// sticky interrupt that outlives its cause, would break these.
const CONTROLS = [
    ["tail-call finite", "function f(n,a){ if (n===0) return a; return f(n-1,a+n); } print(f(200000,0))", "20000100000"],
    ["do-while finite", "var i=0,s=0; do { s+=i; i++; } while (i !== 100000); print(s)", "4999950000"],
    ["tryCatch ordinary", "print((function(){throw new Error('x')}).tryCatch(e=>e.message)())", "x"],
    ["equals tree", "print(Object.equals({a:[1,{b:2}]},{a:[1,{b:2}]}))", "true"],
];
for (const [name, form, want] of CONTROLS) {
    const r = Exec(BIN, ["--timeout-ms", "20000", "-e", form], { timeoutMs: 30000, encoding: "utf8" });
    assert(r.code === 0, name + ": control exited " + r.code + " (" + (r.stderr || "").slice(0, 80) + ")");
    assert((r.stdout || "").trim() === want, name + ": got " + JSON.stringify((r.stdout || "").trim()) + " want " + want);
    print("  ok  control " + name);
}

// E1b-03: a Worker's native stack is smaller than the main thread's on some
// platforms; runaway recursion, deep JSON and deep source inside a Worker
// must each be a catchable error, never a signal.
{
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_w2e.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    let f = std.open(`${T}_child.js`, "w");
    f.puts([
        'import * as os from "os";',
        "function f(n){ return f(n+1)+1; }",
        'let r; try { f(0); r="returned"; } catch(e){ r=e.name; }',
        'let s; try { JSON.parse("[".repeat(200000)); s="parsed"; } catch(e){ s=e.name; }',
        'let t; try { new Function("return " + "(".repeat(200000) + "1" + ")".repeat(200000)); t="compiled"; } catch(e){ t=e.name; }',
        "os.Worker.parent.postMessage({r,s,t});",
    ].join("\n"));
    f.close();
    f = std.open(`${T}_main.mjs`, "w");
    f.puts([
        'import * as os from "os";',
        `const w = new os.Worker(${JSON.stringify(T + "_child.js")});`,
        "w.onmessage = e => { print(JSON.stringify(e.data)); w.onmessage = null; };",
    ].join("\n"));
    f.close();
    const r = Exec(BIN, ["--std", `${T}_main.mjs`], { timeoutMs: 30000, encoding: "utf8" });
    assert(r.code === 0 && r.signal === null, "worker deep recursion: exit " + r.code + " signal " + r.signal);
    let d = {};
    try { d = JSON.parse((r.stdout || "").trim()); } catch (e) { }
    assert(d.r === "RangeError", "worker runaway recursion -> RangeError (got " + d.r + ")");
    assert(d.s === "SyntaxError", "worker deep JSON -> SyntaxError (got " + d.s + ")");
    assert(d.t === "SyntaxError", "worker deep source -> SyntaxError (got " + d.t + ")");
    print("  ok  worker deep recursion is catchable");
}

if (failures) {
    print("test_audit_w2_engine_1: " + failures + " FAILED");
    std.exit(1);
}
print("test_audit_w2_engine_1: all passed");
