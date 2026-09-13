// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["appear-mid => 1,P1,3,P3,5"];
__EXP[1] = ["vanish-mid => 1,U,3,U,5"];
__EXP[2] = ["replace-mid => 1,new,3"];
__EXP[3] = ["delete-ahead => 1,2,P2,4"];
__EXP[4] = ["objproto-mid => 1,U,3,U,5"];
// F05: prototype indexed props appearing (and vanishing) MID-iteration on holey arrays
// spec: holes consult the prototype chain via Get(array, k) each step
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

function clean() { delete P[1]; delete P[3]; }

// proto props appear after the first step — remaining holes pick them up
{
  const a = [1, , 3, , 5];
  const seen = [];
  try {
    for (const v of a) {
      seen.push(v === undefined ? "U" : v);
      if (seen.length === 1) { P[1] = "P1"; P[3] = "P3"; }
    }
  } finally { clean(); }
  __L(0, "appear-mid => " + seen.join(","));
}
// proto prop REMOVED mid-iteration — later hole falls back to undefined
{
  const c = [1, , 3, , 5];
  const seen = [];
  P[1] = "X";
  P[3] = "Y";
  try {
    for (const v of c) {
      seen.push(v === undefined ? "U" : v);
      if (seen.length === 1) { delete P[1]; delete P[3]; }
    }
  } finally { clean(); }
  __L(1, "vanish-mid => " + seen.join(","));
}
// proto prop REPLACED mid-iteration (hole reads the fresh value)
{
  const d = [1, , 3];
  const seen = [];
  P[1] = "old";
  try {
    for (const v of d) {
      seen.push(v === undefined ? "U" : v);
      if (seen.length === 1) P[1] = "new";
    }
  } finally { clean(); }
  __L(2, "replace-mid => " + seen.join(","));
}
// delete-ahead turns an occupied slot into a hole AFTER the cursor passed neighbors
{
  const b = [1, 2, 3, 4];
  const s2 = [];
  P[2] = "P2";
  try {
    for (const v of b) {
      s2.push(v === undefined ? "U" : v);
      if (s2.length === 2) delete b[2];
    }
  } finally { clean(); }
  __L(3, "delete-ahead => " + s2.join(","));
}
// proto indexed prop appears on Object.prototype instead (second link)
{
  Object.prototype[4] = "O4";
  const e = [1, , 3, , 5];
  const seen = [];
  try {
    for (const v of e) {
      seen.push(v === undefined ? "U" : v);
      if (seen.length === 2) delete Object.prototype[4];
    }
  } finally { delete Object.prototype[4]; }
  __L(4, "objproto-mid => " + seen.join(","));
}

summary("arrays_ext");
