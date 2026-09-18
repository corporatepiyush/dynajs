// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a U T"];
__EXP[1] = ["deep T"];
__EXP[2] = ["deep RangeError"];
__EXP[3] = ["b end H,A,H,A,H"];
__REQ = {"dynajs": {"0": 1, "1": 1, "3": 1}, "node": {"0": 1, "2": 1, "3": 1}};
// WHITELIST-DIVERGE: deep-tail-recursion (dynajs PTC succeeds where node RangeErrors — intentional)
// P8: method tail calls with `this` alternating between objects each hop —
// the trampoline must rebind the receiver correctly every frame.
var chainA = { tag: "T", next: null };
chainA.f1 = function (n) { if (n === 0) return this.tag; return this.next.f2(n - 1); };
chainA.f2 = function (n) { if (n === 0) return this.tag; return this.next.f1(n - 1); };
var chainB = { tag: "U", f1: chainA.f1, f2: chainA.f2, next: null };
chainA.next = chainB;
chainB.next = chainA;
__L(0, "a", chainA.f1(5), chainA.f1(6));
try { __L(1, "deep", chainA.f1(200000)); } catch (e) { __L(2, "deep", e.constructor.name); }
// method tail call where `this` is replaced via call() at each level
var acc = [];
var host = {
  tag: "H",
  step(n) {
    acc.push(this.tag);
    if (n === 0) return "end";
    return this.alt.step.call(this.alt, n - 1);
  },
  alt: { tag: "A", alt: null, step(n) {
    acc.push(this.tag);
    if (n === 0) return "end2";
    return host.step.call(host, n - 1);
  } },
};
host.alt.alt = host;
__L(3, "b", host.step(4), acc.join(","));
acc.length = 0;

summary("parser_core_ext");
