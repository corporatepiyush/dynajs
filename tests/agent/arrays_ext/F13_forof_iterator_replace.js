// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["own-gen => x,y spread=[\"x\",\"y\"]"];
__EXP[1] = ["swap-mid => 1,2,3 then=SWAP"];
__EXP[2] = ["get-it => [\"g1\"] twice=[\"g2\"] gets=2"];
__EXP[3] = ["proto-gen => P [\"P\"]"];
__EXP[4] = ["non-callable => TypeError TypeError"];
// F13: own [Symbol.iterator] override — capture happens once at loop start; spread sees it too
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [1, 2, 3];
  a[Symbol.iterator] = function* () { yield "x"; yield "y"; };
  const got = [];
  for (const v of a) got.push(v);
  __L(0, "own-gen => " + got.join(",") + " spread=" + JSON.stringify([...a]));
}
// replace Symbol.iterator MID-iteration: the loop keeps the captured method
{
  const c = [1, 2, 3];
  let swapped = false;
  const got2 = [];
  for (const v of c) {
    got2.push(v);
    if (!swapped) {
      Object.defineProperty(c, Symbol.iterator, {
        value: function* () { yield "SWAP"; }, configurable: true,
      });
      swapped = true;
    }
  }
  const got3 = [];
  for (const v of c) got3.push(v); // new loops use the replacement
  __L(1, "swap-mid => " + got2.join(",") + " then=" + got3.join(","));
}
// getter returning a FRESH generator per Get — spread calls it once per spread
{
  const a = [1, 2];
  let n = 0;
  Object.defineProperty(a, Symbol.iterator, {
    get() { n++; return function* () { yield "g" + n; }; },
    configurable: true,
  });
  __L(2, "get-it => " + JSON.stringify([...a]) + " twice=" + JSON.stringify([...a]) + " gets=" + n);
}
// Symbol.iterator on the PROTOTYPE (not own) — inherited by both for-of and spread
{
  const proto = Object.create(Array.prototype);
  proto[Symbol.iterator] = function* () { yield "P"; };
  const inst = Object.create(proto);
  inst[0] = 1; inst[1] = 2; inst.length = 2;
  let outp = "";
  for (const v of inst) outp += v;
  __L(3, "proto-gen => " + outp + " " + JSON.stringify([...inst]));
}
// Symbol.iterator value non-callable -> TypeError from GetMethod at loop start
{
  const bad = [1];
  bad[Symbol.iterator] = 5;
  let r;
  try { for (const v of bad) {} r = "NO-THROW"; }
  catch (e) { r = e.name; }
  let r2;
  try { [...bad]; r2 = "NO-THROW"; }
  catch (e) { r2 = e.name; }
  __L(4, "non-callable => " + r + " " + r2);
}

summary("arrays_ext");
