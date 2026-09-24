// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["mid-proxy => [10,11,20,21,30] len=5"];
__EXP[1] = ["count => [\"a\",\"b\"] gets>0=true"];
__EXP[2] = ["all-proxy => [0,1,10,11,20,21]"];
__EXP[3] = ["obj-proxy-it => [\"P0\",\"P1\"]"];
__EXP[4] = ["one-get => [\"only1\"] made=1"];
__EXP[5] = ["revoked => [1,2] then=TypeError"];
// S05: proxies mid-spread-chain — [...a, ...b, ...c] with a proxy in the middle; trap counts
const out = typeof console !== "undefined" ? console.log : print;

{
  // transparent proxy over an array in the middle slot
  const t = [20, 21];
  const p = new Proxy(t, {});
  const r = [...[10, 11], ...p, ...[30]];
  __L(0, "mid-proxy => " + JSON.stringify(r) + " len=" + r.length);
}
{
  // proxy with counting get trap (element reads through Get for the iterator path)
  const t2 = ["a", "b"];
  let gets = 0;
  const p2 = new Proxy(t2, {
    get(target, key, rx) { if (typeof key === "string") gets++; return Reflect.get(target, key, rx); },
  });
  const r2 = [...p2];
  __L(1, "count => " + JSON.stringify(r2) + " gets>0=" + (gets > 0));
}
{
  // proxy in EVERY slot, including first and last
  const mk = (v) => new Proxy([v, v + 1], {});
  const r3 = [...mk(0), ...mk(10), ...mk(20)];
  __L(2, "all-proxy => " + JSON.stringify(r3));
}
{
  // proxy whose target is a plain object with a hand-written iterator
  const itObj = new Proxy({}, {
    get(t, key) {
      if (key === Symbol.iterator) return function* () { yield "P0"; yield "P1"; };
      return undefined;
    },
  });
  const r4 = [...itObj];
  __L(3, "obj-proxy-it => " + JSON.stringify(r4));
}
{
  // proxy get trap returning a fresh iterator each Get for @@iterator — spread consumes ONE
  let made = 0;
  const p5 = new Proxy([1], {
    get(t, key, rx) {
      if (key === Symbol.iterator) {
        return () => { made++; return [ "only" + made ][Symbol.iterator](); };
      }
      return Reflect.get(t, key, rx);
    },
  });
  const r5 = [...p5];
  __L(4, "one-get => " + JSON.stringify(r5) + " made=" + made);
}
{
  // revocable proxy revoked between spreads — second spread throws
  const { proxy, revoke } = Proxy.revocable([1, 2], {});
  const first = [...proxy];
  revoke();
  let r6;
  try { r6 = JSON.stringify([...proxy]); } catch (e) { r6 = e.name; }
  __L(5, "revoked => " + JSON.stringify(first) + " then=" + r6);
}

summary("arrays_ext");
