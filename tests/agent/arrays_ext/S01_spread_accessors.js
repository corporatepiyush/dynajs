// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["acc => [1,\"g1\",3] calls=1 len=3"];
__EXP[1] = ["mutget => TypeError mid-spread fired=true blen=4"];
__EXP[2] = ["growget => [1,2,\"L\",\"x\",\"y\"] glen=5"];
__EXP[3] = ["fresh => [\"A!\",\"b\"] [\"A!\",\"b\"]"];
__EXP[5] = ["len-acc => TypeError"];
// S01: spread x accessors — element getters observed per-read, mutating getters, length getter
const out = typeof console !== "undefined" ? console.log : print;

// accessor element: getter called once per spread read, in index order
{
  const a = [1, 2, 3];
  let calls = 0;
  Object.defineProperty(a, "1", { get() { calls++; return "g" + calls; }, configurable: true });
  const r = [...a];
  __L(0, "acc => " + JSON.stringify(r) + " calls=" + calls + " len=" + r.length);
}
// getter that MUTATES the array on read: shift while spreading (latched — no recursion:
// shift() itself reads index 0, so an unlatched mutating getter recurses until overflow)
{
  const b = [1, 2, 3, 4];
  let fired = false;
  Object.defineProperty(b, "0", {
    get() {
      if (fired) return "S";
      fired = true;
      if (b.length > 1) b.shift();
      return "S";
    },
    configurable: true,
  });
  let r2;
  try {
    r2 = JSON.stringify([...b]);
  } catch (e) {
    // shift() inside the getter writes back onto a getter-only slot -> TypeError (both engines)
    r2 = e.name + " mid-spread";
  }
  __L(1, "mutget => " + r2 + " fired=" + fired + " blen=" + b.length);
}
// accessor on the LAST element that appends — array iterator re-reads length each step?
{
  const c = [1, 2, 3];
  Object.defineProperty(c, "2", {
    get() { if (c.length === 3) c.push("x", "y"); return "L"; },
    configurable: true,
  });
  let guard = 0;
  const r3 = [];
  for (const v of c) { r3.push(v); if (++guard > 20) break; }
  __L(2, "growget => " + JSON.stringify(r3) + " glen=" + c.length);
}
// accessor defined but the spread target is a fresh dense array each time — fast path must not skip getters
{
  const mk = () => { const a = ["a", "b"]; Object.defineProperty(a, "0", { get() { return "A!"; }, configurable: true }); return a; };
  __L(3, "fresh => " + JSON.stringify([...mk()]) + " " + JSON.stringify([...mk()]));
}
// length as an accessor (only own, non-configurable allowed per spec — defineProperty on length get throws)
try {
  const d = [1, 2];
  Object.defineProperty(d, "length", { get() { return 99; } });
  __L(4, "len-acc => defined");
} catch (e) { __L(5, "len-acc => " + e.name); }

summary("arrays_ext");
