// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ROW 2024-03-10T07:00:00Z => 1710054000000 | 1710054000000 | 2024-03-10T07:00:00.000Z", "ROW 2024-06-15T12:00:00.500Z => 1718452800500 | 1718452800500 | 2024-06-15T12:00:00.500Z", "ROW 2024-06-15T12:00:00.123+05:45 => 1718432100123 | 1718432100123 | 2024-06-15T06:15:00.123Z", "ROW -271821-04-20T00:00:00Z => -8640000000000000 | -8640000000000000 | -271821-04-20T00:00:00.000Z"];
__EXP[2] = ["SUMMARY fails=0 hash=3d90e7e6"];
// test_date_parse_strict_iso.js — strict ISO parse (Date.parse + new Date(str))
// across formats, DST-boundary instants, extremes, fractional seconds, and
// offsets. Byte-identical vs node + baseline required (strict forms only).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "", fails = 0;
var strings = [
  "2024-03-10T07:00:00Z", "2024-03-10T02:30:00Z",
  "2024-11-03T06:00:00Z", "2024-11-03T05:30:00Z",
  "2024-06-15T12:00:00Z", "2024-06-15T12:00:00.500Z",
  "2024-06-15T12:00:00.05Z", "2024-06-15T12:00:00.000Z",
  "2024-06-15T12:00:00+05:30", "2024-06-15T12:00:00-04:00",
  "2024-06-15T12:00:00.123+05:45",
  "2024-06-15", "2024-06", "2024",
  "+275760-09-13T00:00:00Z", "-271821-04-20T00:00:00Z",
  "1970-01-01T00:00:00Z", "1970-01-01T00:00:00.001Z",
  "2024-02-29T12:00:00Z", "2023-02-29T12:00:00Z" // 2023 invalid day
];
for (var i = 0; i < strings.length; i++) {
  var s = strings[i];
  var p = Date.parse(s);
  var d = new Date(s);
  var line = s + " => " + p + " | " + d.getTime() + " | " + d.toISOString();
  feed += line + "\n";
  if (p !== d.getTime()) { fails++; __L(0, "MISMATCH parse-vs-ctor: " + line); }
  if (i % 5 === 0) __L(1, "ROW " + line);
}
// parse of what toString produces must equal the epoch (round-trip, UTC-independent value)
var t = Date.UTC(2026, 8, 11, 13, 46, 40, 123);
var d2 = new Date(t);
feed += "roundtrip " + Date.parse(d2.toISOString()) + " vs " + t + "\n";
if (Date.parse(d2.toISOString()) !== t) fails++;
// non-strict junk (impl-defined acceptance): record only
var junk = ["", "not a date", "2024-13-01", "2024-06-15T", "2024-06-15T25:00:00Z"];
for (var i = 0; i < junk.length; i++) {
  var v = Date.parse(junk[i]);
  feed += "junk " + junk[i] + " => " + v + "\n";
}
__L(2, "SUMMARY fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
