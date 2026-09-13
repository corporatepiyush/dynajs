// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["grow-mid => 1,2,3,,,,,,,,,,,,,,,,,, len=1000000"];
__EXP[1] = ["sparse8 => H,H,H,H,H,six,H,H"];
__EXP[2] = ["shrink-regrow => 1,2,3,U,U,U,U,U,U,U,ten,U,U,U,U,U,U,U,U,U len=20"];
__EXP[3] = ["bounce => 1,2,3 len=3"];
__EXP[4] = ["early-holes => H,one,H"];
// F04: length set to huge (sparse) mid-iteration — exhaustion transition + sparse Get
// BOUNDED: "huge" = 1e6 (forces sparse in every engine); every loop is break-guarded
// so no probe iterates more than ~16 steps. Original unbounded 2^32-2 form hung per spec.
const out = typeof console !== "undefined" ? console.log : print;
const HUGE = 1000000;

{
  const a = [1, 2, 3];
  const seen = [];
  let guard = 0;
  for (const v of a) {
    seen.push(v);
    if (seen.length === 2) a.length = HUGE; // goes sparse
    if (++guard > 20) break;
  }
  __L(0, "grow-mid => " + seen.join(",") + " len=" + a.length);
}
{
  const b = [];
  b.length = HUGE;
  b[5] = "six";
  let n = 0;
  const got = [];
  for (const v of b) { got.push(v === undefined ? "H" : v); if (++n >= 8) break; }
  __L(1, "sparse8 => " + got.join(","));
}
// shrink below cursor then regrow beyond cursor in one step
{
  const c = [1, 2, 3, 4, 5, 6, 7, 8];
  const s2 = [];
  let guard = 0;
  for (const v of c) {
    s2.push(v === undefined ? "U" : v);
    if (s2.length === 3) { c.length = 2; c.length = 20; c[10] = "ten"; }
    if (++guard > 30) break;
  }
  __L(2, "shrink-regrow => " + s2.join(",") + " len=" + c.length);
}
// huge length then back to small before the next next()
{
  const d = [1, 2, 3, 4];
  const seen = [];
  let guard = 0;
  for (const v of d) {
    seen.push(v);
    if (seen.length === 2) { d.length = HUGE; d.length = 3; }
    if (++guard > 20) break;
  }
  __L(3, "bounce => " + seen.join(",") + " len=" + d.length);
}
// huge sparse length, iterate the FIRST 3 only (holes -> undefined)
{
  const e = [];
  e.length = HUGE;
  e[1] = "one";
  const got = [];
  let n = 0;
  for (const v of e) { got.push(v === undefined ? "H" : v); if (++n >= 3) break; }
  __L(4, "early-holes => " + got.join(","));
}

summary("arrays_ext");
