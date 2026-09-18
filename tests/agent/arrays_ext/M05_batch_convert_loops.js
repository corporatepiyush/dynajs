// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["unshift500 => len=500 desc=true head=[499,498,497] tail=[2,1,0]"];
__EXP[1] = ["shift500 => asc=true left=0"];
__EXP[2] = ["queue => len=20 sum=1190 head=[50,51,52]"];
__EXP[3] = ["conv => first=0 at99=99 at100=100 count=200 asc=true"];
__EXP[4] = ["batch => f=302:0,299,1 h=[0,1,2,0] h3=0 s=0,4,0 sFar@100005=far tag=T"];
__EXP[6] = ["revprefix => ALL-MATCH"];
// M05: convert-or-batch pressure — long unshift/shift/reverse loops across the fast/slow
// boundary (converted results must equal reference semantics every step)
const out = typeof console !== "undefined" ? console.log : print;

{
  // 500 unshifts: a[i] front-inserted; final content must be i descending
  const a = [];
  for (let i = 0; i < 500; i++) a.unshift(i);
  let ok = true;
  for (let i = 0; i < 500; i++) if (a[i] !== 499 - i) { ok = false; break; }
  __L(0, "unshift500 => len=" + a.length + " desc=" + ok + " head=" + JSON.stringify(a.slice(0, 3)) + " tail=" + JSON.stringify(a.slice(-3)));
}
{
  // 500 shifts drain an ascending array; results must be ascending
  const b = [];
  for (let i = 0; i < 500; i++) b.push(i);
  const got = [];
  for (let i = 0; i < 500; i++) got.push(b.shift());
  __L(1, "shift500 => asc=" + got.every((v, i) => v === i) + " left=" + b.length);
}
{
  // alternate unshift/shift (queue) + periodic reverse
  const c = [];
  for (let i = 0; i < 100; i++) {
    c.unshift(i);
    c.push(i);
    if (i % 25 === 0) c.reverse();
  }
  while (c.length > 50) c.shift();
  while (c.length > 20) c.pop();
  let sum = 0;
  for (const v of c) sum += v;
  __L(2, "queue => len=" + c.length + " sum=" + sum + " head=" + JSON.stringify(c.slice(0, 3)));
}
{
  // convert mid-loop: force dictionary elements halfway through shifts
  const d = [];
  for (let i = 0; i < 200; i++) d.push(i);
  const got = [];
  for (let i = 0; i < 200; i++) {
    if (i === 100) { d[1000000] = "boom"; d.length = 100; }
    got.push(d.shift());
  }
  __L(3, "conv => first=" + got[0] + " at99=" + got[99] + " at100=" + got[100] + " count=" + got.length +
      " asc=" + got.every((v, i) => i === 0 || v === got[i - 1] + 1));
}
{
  // unshift MANY args at once (batch path) on fast, holey, slow receivers
  const args = [];
  for (let i = 0; i < 300; i++) args.push(i);
  const f = [0, 1];
  f.unshift(...args);
  const h = [0, , 2];
  h.unshift(...args.slice(0, 3));
  const s = [0];
  s.tag = "T";
  s[100000] = "far";
  s.unshift(...args.slice(0, 5));
  __L(4, "batch => f=" + f.length + ":" + f[0] + "," + f[299] + "," + f[301] +
      " h=" + JSON.stringify(h.slice(0, 4)) + " h3=" + h[3] +
      " s=" + s[0] + "," + s[4] + "," + s[5] + " sFar@" + (100005) + "=" + s[100005] + " tag=" + s.tag);
}
{
  // reverse on every prefix — results match Array.prototype.slice().reverse() oracle
  const base = [];
  for (let i = 0; i < 50; i++) base.push(i);
  let ok = true;
  for (let n = 0; n <= 50; n += 5) {
    const p = base.slice(0, n);
    p.reverse();
    const oracle = base.slice(0, n).slice().reverse();
    if (JSON.stringify(p) !== JSON.stringify(oracle)) { ok = false; __L(5, "MISMATCH at n=" + n); break; }
  }
  __L(6, "revprefix => " + (ok ? "ALL-MATCH" : "MISMATCH"));
}

summary("arrays_ext");
