// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["SUMMARY fails=0 hash=6a18515c"];
// test_date_epoch_extremes.js — ±8.64e15 ladder: exact boundary valid, ±1ms
// invalid; fields at ±275760 extremes; toISOString at the limits; setTime clip.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "", fails = 0;
function chk(cond, label) { if (!cond) { fails++; __L(0, "FAIL " + label); } }
var MAX = 8.64e15;
chk(new Date(MAX).getTime() === MAX, "max-valid");
chk(new Date(-MAX).getTime() === -MAX, "min-valid");
var over = new Date(MAX + 1);
chk(over.getTime() !== over.getTime(), "max+1-invalid");
var under = new Date(-MAX - 1);
chk(under.getTime() !== under.getTime(), "min-1-invalid");
// setTime clip on a live object
var d = new Date(0);
d.setTime(MAX); chk(d.getTime() === MAX, "settime-max");
d.setTime(MAX + 1); chk(d.getTime() !== d.getTime(), "settime-max+1");
d.setTime(-MAX); chk(d.getTime() === -MAX, "settime-min");
// fields at extremes (UTC, unambiguous)
var hi = new Date(MAX);
feed += "hi " + hi.toISOString() + " Y=" + hi.getUTCFullYear() + " M=" + hi.getUTCMonth() +
        " D=" + hi.getUTCDate() + " day=" + hi.getUTCDay() + "\n";
chk(hi.getUTCFullYear() === 275760 && hi.getUTCMonth() === 8 && hi.getUTCDate() === 13, "max-fields");
var lo = new Date(-MAX);
feed += "lo " + lo.toISOString() + " Y=" + lo.getUTCFullYear() + " M=" + lo.getUTCMonth() +
        " D=" + lo.getUTCDate() + " day=" + lo.getUTCDay() + "\n";
chk(lo.getUTCFullYear() === -271821 && lo.getUTCMonth() === 3 && lo.getUTCDate() === 20, "min-fields");
// ms ladder at the edge: last 100 valid values
for (var m = 0; m < 100; m++) {
  var v = new Date(MAX - m);
  if (v.getTime() !== MAX - m) { fails++; __L(1, "FAIL edge-ladder " + m); }
  if (m === 0 || m === 99) feed += "edge " + v.toISOString() + "\n";
}
// local getters at extremes must be finite (offset ±<1d cannot clip)
feed += "hiLocal Y=" + hi.getFullYear() + " off=" + hi.getTimezoneOffset() + "\n";
feed += "loLocal Y=" + lo.getFullYear() + " off=" + lo.getTimezoneOffset() + "\n";
// ISO round-trip of extremes
chk(Date.parse(hi.toISOString()) === MAX, "iso-roundtrip-max");
chk(Date.parse(lo.toISOString()) === -MAX, "iso-roundtrip-min");
__L(2, "SUMMARY fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
