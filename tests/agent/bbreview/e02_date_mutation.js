// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~mut1", "~mut1"]];
__EXP[1] = ["mut2 2464000000000 2048-01-30T12:26:40.000Z"];
__EXP[2] = [["~mut3 5190005", "~mut3 5190005"]];
__EXP[3] = [["~mut4 100", "~mut4 100"]];
// E: getFullYear loop on one Date while another is mutated in parallel
const reader = new Date(1699999999999);
const writer = new Date(1600000000000);
const seen = new Set();
for (let i = 0; i < 10000; i++) {
  seen.add(reader.getFullYear());
  writer.setTime(writer.getTime() + 86400000);
  if (seen.size > 3) break;
}
__L(0, "mut1", seen.size, [...seen].join(","));
__L(1, "mut2", writer.getTime(), writer.toISOString());
const a = new Date(0), b = new Date(0);
let s = 0;
for (let i = 0; i < 5000; i++) {
  a.setTime(i);
  b.setTime(1000000 - i);
  s += a.getMilliseconds() + b.getMilliseconds() + a.getSeconds() + b.getSeconds();
}
__L(2, "mut3", s, a.getTime(), b.getTime());
// writer becomes invalid then valid
const c = new Date(1700000000000);
const r2 = new Date(1700000000000);
let bad = 0;
for (let i = 0; i < 100; i++) {
  c.setTime(NaN);
  if (isNaN(c.getFullYear())) bad++;
  c.setTime(1700000000000 + i);
  if (c.getFullYear() === 2023) bad += 0;
}
__L(3, "mut4", bad, isNaN(c.getTime()) ? "is-nan" : "valid", r2.getFullYear());

summary("bbreview");
