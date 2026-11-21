// flags: --std
// timeout: 600
// covers: c:src/parser/parser.inc.c
// tests/core/test_audit_w2_engine_1.js -- --timeout-ms must hold for every loop
// shape the interpreter can form, and no built-in may swallow the interrupt.
// SEC-01 (tail calls), E2c-01 (fused compare-and-branch back-edges), E5-01
// (Function.prototype.tryCatch), E5-05 (Object.equals on a shared DAG),
// E1b-03 (deep recursion inside a Worker must be a catchable error). Deep JSON is a SyntaxError on every
// platform: the JSON tokenizer and json_parse_value both report a stack overflow as SyntaxError; which check
// trips first depends on frame layout (glibc worker threads hit json_parse_value and got RangeError).
// Each form runs in a child dynajs so the deadline is armed per form.
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_parser_inc_audit_w2_engine_1");
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

__kitS.section("import * as os from \"os\";", () => {
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
});
__kitS.end();

if (failures) {
    print("test_audit_w2_engine_1: " + failures + " FAILED");
    std.exit(1);
}
print("test_audit_w2_engine_1: all passed");
