// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["0 225 153", "1 743 461", ["~2", "~2"], "3 840 520", "4 1130 693", "5 413 249", "6 43 37", ["~7", "~7"]];
__EXP[1] = ["done"];
let s = 12345n;
function rnd(n){ s = (s * 6364136223846793005n + 1442695040888963407n) & 0xffffffffffffffffn; return s >> (64n - BigInt(n)); }
for (let t = 0; t < 8; t++) {
  const bits = 1 + Number(rnd(11) % 5000n);
  let a = 0n, b = 0n;
  for (let i = 0; i < bits/32; i++) { a = (a << 32n) | (rnd(32) & 0xffffffffn); b = (b << 32n) | (rnd(32) & 0xffffffffn); }
  a = a >> 1n; b = b >> 1n;
  if (rnd(1)) a = -a;
  if (rnd(1)) b = -b;
  if (a === 0n) a = 5n;
  const p = a * b;
  const st = p.toString();
  __L(0, t, bits, st.length);
}
__L(1, "done");

summary("ev_bigint");
