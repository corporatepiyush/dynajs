// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["sum=-61800 hash=29cb1d64"];
// test_date_mut_settime_iteration.js — setTime during iteration over
// date-bearing objects: forEach, for-of over copies, Map.forEach, indexed
// object values, then JSON.stringify (toJSON path) of the mutated dates.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var arr = [];
for (var i = 0; i < 200; i++) arr.push(new Date(1700000000000 + i * 86400000));
// forEach mutation
arr.forEach(function (d, i) {
  d.setTime(d.getTime() + 3600000 * (i % 24));
  feed += d.getTime() + "," + d.getHours() + ";" ;
});
feed += "\n";
// for-of over a copy, setTime each
var copy = arr.slice();
for (var d of copy) { d.setTime(1710000000000); feed += d.getTime() + ";"; }
feed += "\n";
// Map of dates mutated during forEach
var mp = new Map();
for (var i = 0; i < 50; i++) mp.set("k" + i, new Date(1600000000000 + i * 1000));
mp.forEach(function (v, k) { v.setTime(v.getTime() - 60000); feed += k + ":" + v.getTime() + "," + v.getMinutes() + ";"; });
feed += "\n";
// plain object with date values, mutate through Object.keys walk
var o = {};
for (var i = 0; i < 50; i++) o["p" + i] = new Date(1650000000000 + i * 1000);
var keys = Object.keys(o);
for (var i = 0; i < keys.length; i++) { o[keys[i]].setTime(1000 + i); feed += o[keys[i]].getTime() + ";" + o[keys[i]].getMilliseconds() + ";"; }
feed += "\n";
// interleaved getters during iteration (cache reuse under iteration)
var sum = 0;
copy.forEach(function (d, i) { sum = (sum + d.getHours() + d.getMilliseconds() + d.getTimezoneOffset()) % 1000003; });
// JSON.stringify of mutated dates (toJSON -> ISO)
feed += "JSON:" + JSON.stringify(copy.slice(0, 5)) + "\n";
feed += "JSONMAP:" + JSON.stringify([copy[0], {d: copy[1], n: 2}, [copy[2]]]) + "\n";
__L(0, "sum=" + sum + " hash=" + fnv(feed));

summary("builtins_ext");
