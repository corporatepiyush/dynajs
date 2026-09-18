// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["alt => len=1500 sum=1250000 spreadSum=250000 head=[0,2,3,2,6] tail=[1994,1995,998,1998,1999]"];
__EXP[1] = ["slow-alt => len=101501 at100000=1 first500=[null,null,null] expectedLen=101501 match=true"];
__EXP[2] = ["mix => len=1011 sum=5050 55*100=5500"];
__EXP[3] = ["grow => len=234 [0,1,1,2,2,2,3,3] all-i-match=true"];
// B02: 1000 alternating push / spread-append — array must survive fast<->slow oscillation
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [];
  let spreadSum = 0;
  for (let i = 0; i < 1000; i++) {
    if (i % 2 === 0) {
      a.push(i);
    } else {
      const chunk = [i * 2, i * 2 + 1];
      a.push(...chunk);
      spreadSum += i;
    }
    if (i % 250 === 0) { a.length = a.length; a[a.length] = -i; a.pop(); }
  }
  __L(0, "alt => len=" + a.length + " sum=" + a.reduce((s, v) => s + v, 0) +
      " spreadSum=" + spreadSum + " head=" + JSON.stringify(a.slice(0, 5)) +
      " tail=" + JSON.stringify(a.slice(-5)));
}
{
  // force slow (dictionary) mid-way, keep alternating
  const b = [0];
  b[100000] = 1;
  for (let i = 0; i < 1000; i++) {
    if (i % 2 === 0) b.push(i);
    else b.push(...[i, i]);
  }
  const expectedLen = 1 + 1 + 100000 - 1 + 500 + 1000;
  __L(1, "slow-alt => len=" + b.length + " at100000=" + b[100000] +
      " first500=" + JSON.stringify(b.slice(1, 4)) + " expectedLen=" + expectedLen +
      " match=" + (b.length === expectedLen));
}
{
  // alternate push(...big) and unshift
  const c = [0];
  for (let i = 0; i < 100; i++) {
    c.push(...[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    if (i % 10 === 0) c.unshift(-i);
  }
  let sum = 0;
  for (const v of c) sum += v;
  __L(2, "mix => len=" + c.length + " sum=" + sum + " 55*100=" + (55 * 100));
}
{
  // spread into an array being grown past dense capacity repeatedly
  const d = [];
  for (let i = 0; i < 60; i++) {
    d.push(...new Array(i % 7 + 1).fill(i));
  }
  __L(3, "grow => len=" + d.length + " " + JSON.stringify(d.slice(0, 8)) + " all-i-match=" +
      d.every((v, i) => v <= Math.floor(i) + 6));
}

summary("arrays_ext");
