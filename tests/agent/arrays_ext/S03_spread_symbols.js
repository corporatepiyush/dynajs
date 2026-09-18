// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["excluded => [1,2,3] len=3 hasK1=true hasFoo=true"];
__EXP[1] = ["included => symbol true true len=3"];
__EXP[2] = ["sym-acc => [1] reads=0"];
__EXP[3] = ["named-excluded => [4,5] true"];
__EXP[4] = ["wkt => [1] tag=undefined"];
// S03: symbol-KEYED properties are EXCLUDED from spread; symbol-VALUED elements are INCLUDED
const out = typeof console !== "undefined" ? console.log : print;

{
  const s1 = Symbol("k1"), s2 = Symbol("k2");
  const a = [1, 2, 3];
  a[s1] = "via-sym-key";
  a[-1] = "negative-key";
  a["1.5"] = "fraction-key";
  a["foo"] = "named";
  const r = [...a];
  __L(0, "excluded => " + JSON.stringify(r) + " len=" + r.length +
      " hasK1=" + (r[s1] === undefined) + " hasFoo=" + (r.foo === undefined));
}
{
  // symbol-VALUED elements: spread copies them
  const sym = Symbol("v");
  const b = [sym, 1, sym];
  const r2 = [...b];
  __L(1, "included => " + typeof r2[0] + " " + (r2[0] === sym) + " " + (r2[2] === sym) + " len=" + r2.length);
}
{
  // symbol-keyed element with an accessor — must NOT be read by spread
  const s3 = Symbol("acc");
  let reads = 0;
  const c = [1];
  Object.defineProperty(c, s3, { get() { reads++; return "X"; }, configurable: true, enumerable: true });
  const r3 = [...c];
  __L(2, "sym-acc => " + JSON.stringify(r3) + " reads=" + reads);
}
{
  // enumerable string-keyed named props are also excluded (indexed only)
  const d = [4, 5];
  d.extra = "E";
  __L(3, "named-excluded => " + JSON.stringify([...d]) + " " + ([...d].extra === undefined));
}
{
  // well-known symbols on the instance don't leak except @@iterator (consumed, not copied)
  const e = [1];
  e[Symbol.toStringTag] = "T";
  const r5 = [...e];
  __L(4, "wkt => " + JSON.stringify(r5) + " tag=" + r5[Symbol.toStringTag]);
}

summary("arrays_ext");
