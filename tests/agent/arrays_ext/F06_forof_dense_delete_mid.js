__EXP = {};
__EXP[0] = ["del-noproto => 1,2,U,4,5"];
__EXP[1] = ["del-proto => 1,2,D2,4"];
__EXP[2] = ["own-wins => 1,2,3"];
__EXP[3] = ["del-all => 1,U,U,U,U"];
__EXP[4] = ["del-current => 1,2,3 len=3 keys=0"];
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

{
  const a = [1, 2, 3, 4, 5];
  const seen = [];
  for (const v of a) {
    seen.push(v === undefined ? "U" : v);
    if (seen.length === 2) delete a[2];
  }
  __L(0, "del-noproto => " + seen.join(","));
}
{
  const a = [1, 2, 3, 4];
  const seen = [];
  P[2] = "D2";
  try {
    for (const v of a) {
      seen.push(v === undefined ? "U" : v);
      if (seen.length === 1) delete a[2];
    }
  } finally { delete P[2]; }
  __L(1, "del-proto => " + seen.join(","));
}
{
  const a = [1, 2, 3];
  const seen = [];
  P[1] = "P1";
  try {
    for (const v of a) seen.push(v === undefined ? "U" : v);
  } finally { delete P[1]; }
  __L(2, "own-wins => " + seen.join(","));
}
{
  const a = [1, 2, 3, 4, 5];
  const seen = [];
  for (const v of a) {
    seen.push(v === undefined ? "U" : v);
    if (seen.length === 1) { delete a[1]; delete a[2]; delete a[3]; delete a[4]; }
  }
  __L(3, "del-all => " + seen.join(","));
}
{
  const a = [1, 2, 3];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    delete a[seen.length - 1];
  }
  __L(4, "del-current => " + seen.join(",") + " len=" + a.length + " keys=" + Object.keys(a).length);
}

summary("arrays_ext");
