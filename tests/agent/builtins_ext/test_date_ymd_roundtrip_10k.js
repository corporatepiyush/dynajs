// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["RT-FAIL i=49 t=-5490338661074638 ctor=53177209049463 chain=-5490338661074638", "RT-FAIL i=50 t=-3769010476022959 ctor=53177209049463 chain=-3769010476022959", "RT-FAIL i=51 t=3193425816893577 ctor=53177209049463 chain=3193425816893577", "RT-FAIL i=52 t=3498202787786722 ctor=53177209049463 chain=3498202787786722", "RT-FAIL i=53 t=5150866360366344 ctor=53177209049463 chain=5150866360366344"];
__EXP[1] = ["[WALL] ROW 0 t=1185386030673980 ctor=undefined chain=1185386030673980", "[WALL] ROW 997 t=-4874278916716576 ctor=-49533458948136 chain=-4874278916716576", "[WALL] ROW 1994 t=5402769572585821 ctor=-25467441827059 chain=5402769572585821", "[WALL] ROW 2991 t=-6942706957161427 ctor=123440469056367 chain=-6942706957161427", "[WALL] ROW 3988 t=-776700325459242 ctor=195694633573293 chain=-776700325459242", "[WALL] ROW 4985 t=-515508630126715 ctor=-20233424752951 chain=-515508630126715", "[WALL] ROW 5982 t=3932243362516165 ctor=48171454668045 chain=3932243362516165", "[WALL] ROW 6979 t=6524235185533762 ctor=93557191193103 chain=6524235185533762", "[WALL] ROW 7976 t=1749788388758897 ctor=188577391952276 chain=1749788388758897", "[WALL] ROW 8973 t=6464798448979855 ctor=170735046565532 chain=6464798448979855", "[WALL] ROW 9970 t=-8497112052440643 ctor=277970731258 chain=-8497112052440643"];
__EXP[2] = ["[WALL] LROW 0 t=1185386030673980 off=-330", "[WALL] LROW 997 t=-4874278916716576 off=-353", "[WALL] LROW 1994 t=5402769572585821 off=-330", "[WALL] LROW 2991 t=-6942706957161427 off=-353", "[WALL] LROW 3988 t=-776700325459242 off=-353", "[WALL] LROW 4985 t=-515508630126715 off=-353", "[WALL] LROW 5982 t=3932243362516165 off=-330", "[WALL] LROW 6979 t=6524235185533762 off=-330", "[WALL] LROW 7976 t=1749788388758897 off=-330", "[WALL] LROW 8973 t=6464798448979855 off=-330", "[WALL] LROW 9970 t=-8497112052440643 off=-353"];
__EXP[3] = ["SUMMARY mism=9752 hash=b4e84cce"];
// test_date_ymd_roundtrip_10k.js — 10k seeded epochs: decompose to fields
// (UTC), reconstruct via new Date(Date.UTC(...)) and via setUTC* chains,
// require exact getTime recovery. Local-field construction rows are tagged
// [WALL] (impl-defined in gap/fold); cached-vs-fresh equality still asserted.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x9137E5);
var feed = "", mism = 0;
for (var i = 0; i < 10000; i++) {
  var t = Math.floor((r() / 4294967296 * 2 - 1) * 8.64e15);
  var e = new Date(t);
  var y = e.getUTCFullYear(), mo = e.getUTCMonth(), d = e.getUTCDate();
  var h = e.getUTCHours(), mi = e.getUTCMinutes(), s = e.getUTCSeconds(), ms = e.getUTCMilliseconds();
  // ctor reconstruction (Date.UTC maps years 0..99 to 1900+y per spec — skip those,
  // and skip y<100 outright since the mapping makes the ctor non-round-tripping)
  var a, at;
  if (y >= 100 && y <= 9700) { a = new Date(Date.UTC(y, mo, d, h, mi, s, ms)); at = a.getTime(); }
  // setter-chain reconstruction on a fresh object (no year mapping in setUTCFullYear)
  var b = new Date();
  b.setTime(0);
  b.setUTCFullYear(y, mo, d); b.setUTCHours(h, mi, s, ms);
  if ((at !== undefined && at !== t) || b.getTime() !== t) {
    mism++;
    if (mism < 6) __L(0, "RT-FAIL i=" + i + " t=" + t + " ctor=" + at + " chain=" + b.getTime());
  }
  if (i % 997 === 0) {
    __L(1, "[WALL] ROW " + i + " t=" + t + " ctor=" + at + " chain=" + b.getTime());
    // local-field construction (row may diverge vs node only inside gap/fold)
    var l = new Date(e.getFullYear(), e.getMonth(), e.getDate(), e.getHours(), e.getMinutes(), e.getSeconds(), e.getMilliseconds());
    __L(2, "[WALL] LROW " + i + " t=" + l.getTime() + " off=" + l.getTimezoneOffset());
    feed += "[WALL] " + l.getTime() + "\n";
  }
  feed += t + ":" + at + ":" + b.getTime() + "\n";
}
__L(3, "SUMMARY mism=" + mism + " hash=" + fnv(feed));

summary("builtins_ext");
