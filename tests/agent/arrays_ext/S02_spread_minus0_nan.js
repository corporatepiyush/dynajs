// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["literals => -0,NaN,0,Infinity,-Infinity,Infinity"];
__EXP[1] = ["getters => -0,NaN"];
__EXP[2] = ["push => -0,NaN,-0,NaN len=4"];
__EXP[3] = ["mutate => 0,NaN,-0,-0"];
__EXP[4] = ["concat => -0,NaN,0"];
__EXP[5] = ["slice => -0,NaN slice0 => -0"];
__EXP[6] = ["frozen => -0,NaN"];
// S02: spread x numeric identity — -0 and NaN pass through spread without normalization
const out = typeof console !== "undefined" ? console.log : print;

const tag = (v) => Object.is(v, -0) ? "-0" : (Number.isNaN(v) ? "NaN" : (v === undefined ? "U" : String(v)));

{
  const a = [-0, NaN, 0, Infinity, -Infinity, 1.5e308 * 10];
  const r = [...a];
  __L(0, "literals => " + r.map(tag).join(","));
}
{
  // -0 and NaN produced by getters
  const b = [-0, NaN];
  Object.defineProperty(b, "0", { get() { return -0; }, configurable: true });
  Object.defineProperty(b, "1", { get() { return NaN; }, configurable: true });
  __L(1, "getters => " + [...b].map(tag).join(","));
}
{
  // -0 through push(...spread) bulk append
  const c = [];
  c.push(-0, NaN);
  c.push(...[-0, NaN]);
  __L(2, "push => " + [...c].map(tag).join(",") + " len=" + c.length);
}
{
  // unshift/reverse round trip preserves identity
  const d = [-0, NaN, 0];
  d.unshift(-0);
  d.reverse();
  __L(3, "mutate => " + d.map(tag).join(","));
}
{
  // concat preserves
  const e = [-0].concat([NaN], [0]);
  __L(4, "concat => " + e.map(tag).join(","));
}
{
  // slice of -0/NaN
  const f = [0, -0, NaN, 0];
  __L(5, "slice => " + f.slice(1, 3).map(tag).join(",") + " slice0 => " + tag(f.slice(1, 2)[0]));
}
{
  // Object.freeze then spread
  const g = Object.freeze([-0, NaN]);
  __L(6, "frozen => " + [...g].map(tag).join(","));
}

summary("arrays_ext");
