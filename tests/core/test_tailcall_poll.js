// covers: c:src/vm/tailcall.inc.c
// timeout: 120
// Backward conditional branches must poll for interrupts in both interpreter
// modes (computed goto and CONFIG_TAILCALL_INTERP=y, src/vm/tailcall.inc.c).
// Each form below loops forever through one fused or plain conditional
// branch (if_true/if_true8/if_false, strict_(n)eq_if_true, the compare
// branches) with no goto in the loop, so only that branch's poll can stop it;
// the tail-call forms loop through OP_tail_call(_method) frame reuse only,
// with no branch at all, so only the poll on the call path can stop them;
// the call-tree forms (no loop, no tail call: plain calls, method calls, new)
// make 2^60 calls through frames pushed by the call handlers, so only the
// poll on a frame push can stop them. The callback forms make one call of
// forEach/reduce on an 8e6-element array whose callback has no loop and no
// call, so only the per-callback poll of the in-interpreter array loop
// (js_tc_iter_step) can stop them before the array ends.
// A child dynajs runs each form under --timeout-ms; it must die with an
// interrupt promptly. Proven by planting "never poll on a backward
// if_true/if_false" in the tail-call handlers: the do-while forms hang
// until the parent's kill and this suite goes red; likewise dropping the
// poll from the inline-call frame push (call-tree forms) or from the inline
// tail-call frame reuse (tail-call forms), or from the in-interpreter array
// loop's callback entry (callback forms: the child then finishes the array
// without an interrupt).
// The child is judged by how it ended (interrupted, not killed at the
// parent's deadline), never by a wall-clock budget.
// No form may retain a growing heap: the constructor tree once stored each
// child (this.a = new C(..)), so the live tree reached ~180 MB by the 300 ms
// deadline and each GC cycle (which never polls) took 70 ms unloaded and up
// to 265 ms with 14 copies on 10 cores, past the watchdog's 250 ms hard-stop
// grace: "did not stop" with no poll missing (14x20 runs: 17 and 18 such
// kills with and without the in-interpreter array loop, 0 for the
// non-retaining form, whose RSS stays ~5 MB).
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { Exec, args } from "dyna:sys";
import { sections } from "../lib/kit.js";

const BIN = args()[0];
const pad = "t = t + 1; t = t - 1; ".repeat(12);
const cbBody = "s = (s + v) | 0; s = (s ^ v) | 0; ".repeat(20);
const FORMS = [
    ["do-while truthy", "let t = 1; do { t = t | 1; } while (t);"],
    ["do-while long body", "let t = 1; do { " + pad + " } while (t);"],
    ["do-while negated", "let f = false, t = 0; do { t = t ^ 1; } while (!f);"],
    ["do-while strict neq", "let i = 0; do { i = (i + 1) & 1023; } while (i !== -1);"],
    ["do-while strict eq", "let i = 0; do { i = (i + 1) & 1023; } while (i === i);"],
    ["do-while lt", "let i = 0; do { i = (i + 1) & 1023; } while (i < 2000);"],
    ["do-while lte float", "let i = 0.5; do { i = i * 1; } while (i <= 2.5);"],
    ["do-while gt", "let i = 5; do { i = (i + 1) & 1023; } while (i > -1);"],
    ["do-while gte", "let i = 5; do { i = (i + 1) & 1023; } while (i >= 0);"],
    ["for lt", "for (let i = 0; i < 1e300; i = i + 0) {}"],
    ["tail-call recursion", "\"use strict\"; function f(n) { return f(n + 1); } f(0);"],
    ["tail-call method recursion", "\"use strict\"; const o = { f(n) { return this.f(n + 1); } }; o.f(0);"],
    ["calls in a loop", "function g(x) { return x + 1; } let t = 0; do { t = g(t) & 1023; } while (t !== -1);"],
    ["call tree", "function f(n) { if (n === 0) return 0; return f(n - 1) + f(n - 1) + 1; } f(60);"],
    ["method call tree", "const o = { f(n) { if (n === 0) return 0; return this.f(n - 1) + this.f(n - 1) + 1; } }; o.f(60);"],
    ["fused method call tree", "const o = { f(k) { const m = k - 1; if (k === 0) return 0; return o.f(m) + o.f(m) + 1; } }; o.f(60);"],
    ["constructor tree", "function C(n) { if (n > 0) { new C(n - 1); new C(n - 1); } } new C(60);"],
    ["forEach callbacks", "(function () { const a = new Array(8e6).fill(1); let s = 0; const f = (v) => { " + cbBody + " }; a.forEach(f); })();"],
    ["reduce callbacks", "const a = new Array(8e6).fill(1); a.reduce((s, v) => { " + cbBody + " return s; }, 0);"],
];
let failures = 0;
const S = sections("test_tailcall_poll");
for (const [name, form] of FORMS) S.section(name + " is interrupted under --timeout-ms", () => {
    const r = Exec(BIN, ["--timeout-ms", "300", "-e", form], { timeoutMs: 8000, encoding: "utf8" });
    const out = r.stdout + r.stderr;
    const ok = !r.timedOut && /interrupt/i.test(out);
    if (!ok) {
        failures++;
        print("  FAIL " + name + ": timedOut=" + r.timedOut + " out=" + out.slice(0, 80));
    }
});
S.end();
if (failures) throw new Error("test_tailcall_poll: " + failures + " forms not interrupted");
print("ok test_tailcall_poll (" + FORMS.length + " forms)");
