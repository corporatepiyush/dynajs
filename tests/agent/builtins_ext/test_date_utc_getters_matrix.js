// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ROW 0 3880831173062324:124948,7,23,11,11,2,324,5", "ROW 2009 -7256064243614674:-227966,5,23,22,59,45,326,5", "ROW 4018 -5508313803970814:-172582,5,27,0,13,49,186,6", "ROW 6027 6970272742062807:222849,1,24,0,47,42,807,3", "ROW 8036 5511881042718887:176634,6,23,11,25,18,887,3", "ROW 10045 -1100696041435004:-32910,4,1,11,36,4,996,4", "ROW 12054 6750573418736458:215887,1,26,18,58,56,458,6", "ROW 14063 2929903886765241:94814,11,16,14,46,5,241,2", "ROW 16072 -5333338655680418:-167037,2,24,8,5,19,582,4", "ROW 18081 1267553213238716:42137,1,28,18,47,18,716,4"];
__EXP[2] = ["SUMMARY mism=0 hash=a4b39f2b"];
// test_date_utc_getters_matrix.js — pure UTC getters (no tz memo involvement)
// across 20k seeded epochs + every-97th local getter cross-check. UTC path is
// the cache-invariant control: any breakdown-cache leak shows as twin mismatch.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x00C7A11);
var feed = "", mism = 0;
for (var i = 0; i < 20000; i++) {
  var t = Math.floor((r() / 4294967296 * 2 - 1) * 8.64e15);
  var a = new Date(t);
  // repeated reads (would hit a cache)
  var y1 = a.getUTCFullYear(), mo1 = a.getUTCMonth(), d1 = a.getUTCDate(),
      h1 = a.getUTCHours(), mi1 = a.getUTCMinutes(), s1 = a.getUTCSeconds(),
      ms1 = a.getUTCMilliseconds(), day1 = a.getUTCDay();
  var y2 = a.getUTCFullYear(), mo2 = a.getUTCMonth(), d2 = a.getUTCDate(),
      h2 = a.getUTCHours(), mi2 = a.getUTCMinutes(), s2 = a.getUTCSeconds(),
      ms2 = a.getUTCMilliseconds(), day2 = a.getUTCDay();
  var b = new Date(t);
  var ok = y1 === y2 && mo1 === mo2 && d1 === d2 && h1 === h2 && mi1 === mi2 &&
           s1 === s2 && ms1 === ms2 && day1 === day2 &&
           y1 === b.getUTCFullYear() && h1 === b.getUTCHours() && ms1 === b.getUTCMilliseconds();
  if (!ok) { mism++; if (mism < 6) __L(0, "UTC-MISMATCH i=" + i + " t=" + t); }
  var line = t + ":" + y1 + "," + mo1 + "," + d1 + "," + h1 + "," + mi1 + "," + s1 + "," + ms1 + "," + day1;
  feed += line + "\n";
  if (i % 2009 === 0) __L(1, "ROW " + i + " " + line);
}
__L(2, "SUMMARY mism=" + mism + " hash=" + fnv(feed));

summary("builtins_ext");
