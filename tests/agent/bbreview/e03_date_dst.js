// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1678563000000/1/2023-3-12\n1678570200000/3/2023-3-12\n1699126200000/1/2023-11-5\n1699133400000/3/2023-11-5\n1678566600000/2/2023-3-12\n-37800000/19/1969-12-31\n2147463000000/3/2038-1-19"];
__EXP[1] = ["amb 1699128000000 1"];
__EXP[2] = ["x1 12 -330"];
__EXP[3] = ["x2 12 -330"];
__EXP[4] = ["rt 1710053940000 12 -330 true", "rt 1710054060000 12 -330 true", "rt 1730523540000 10 -330 true", "rt 1730527260000 11 -330 true"];
__EXP[5] = ["seth 1678575600000 4"];
__EXP[6] = ["seth2 1678579200000 5"];
__EXP[7] = ["off-winter -330 off-summer -330"];
// E: component-constructed dates across US DST boundaries (TZ=America/New_York)
const out = [];
for (const [y, mo, d, h] of [
  [2023, 2, 12, 1], [2023, 2, 12, 3],
  [2023, 10, 5, 1], [2023, 10, 5, 3],
  [2023, 2, 12, 2],
  [1969, 11, 31, 19], [2038, 0, 19, 3],
]) {
  const dt = new Date(y, mo, d, h, 0, 0);
  out.push(dt.getTime() + "/" + dt.getHours() + "/" + dt.getFullYear() + "-" + (dt.getMonth() + 1) + "-" + dt.getDate());
}
__L(0, out.join("\n"));
__L(1, "amb", new Date(2023, 10, 5, 1, 30, 0).getTime(), new Date(2023, 10, 5, 1, 30, 0).getHours());
const dt2 = new Date(Date.UTC(2023, 2, 12, 6, 59, 59));
__L(2, "x1", dt2.getHours(), dt2.getTimezoneOffset());
const dt3 = new Date(Date.UTC(2023, 2, 12, 7, 0, 1));
__L(3, "x2", dt3.getHours(), dt3.getTimezoneOffset());
// round-trip across both 2024 boundaries
for (const utc of [Date.UTC(2024, 2, 10, 6, 59, 0), Date.UTC(2024, 2, 10, 7, 1, 0), Date.UTC(2024, 10, 2, 4, 59, 0), Date.UTC(2024, 10, 2, 6, 1, 0)]) {
  const d = new Date(utc);
  __L(4, "rt", utc, d.getHours(), d.getTimezoneOffset(), d.toString() === d.toString());
}
// setHours across the DST gap
const sd = new Date(2023, 2, 12, 1, 30, 0);
sd.setHours(4);
__L(5, "seth", sd.getTime(), sd.getHours());
sd.setTime(sd.getTime() + 3600000);
__L(6, "seth2", sd.getTime(), sd.getHours());
__L(7, "off-winter", new Date(2023, 0, 15).getTimezoneOffset(), "off-summer", new Date(2023, 6, 15).getTimezoneOffset());

summary("bbreview");
