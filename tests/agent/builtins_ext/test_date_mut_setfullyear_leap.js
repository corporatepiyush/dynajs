// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["[LMT] ROW 1899:0:1899,2,1,17,3:-2235384000000", "[LMT] ROW 2000:1:2000,2,1,17,3:951912000000", "[LMT] ROW 2025:2:2025,2,1,17,6:1740830400000", "[LMT] ROW 2399:3:2399,1,1,17,1:13540651200000", ["[LMT] ROW 1901:4:1901,1,1,17,5:-2174730660000", "[LMT] ROW 1901:4:1901,1,1,17,5:-2174730670000"]];
__EXP[3] = ["SUMMARY mism=0 hashModern=811c9dc5"];
__EXP[4] = [["[LMT] hashAll=b892deca hashOld=b892deca", "[LMT] hashAll=42ddb7d2 hashOld=42ddb7d2"]];
__REQ = {"dynajs": {"0": 5, "3": 1, "4": 1}, "node": {"0": 5, "3": 1, "4": 1}};
// test_date_mut_setfullyear_leap.js — setFullYear on Feb 29 across 400 years of
// leap candidates: Feb29.setFullYear(nonLeap) must land Mar 1; chains through
// 2000/1900/2100 century rules; cached vs fresh twin after each step.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "", mism = 0, feedModern = "", feedOld = "", sticky = false;
var years = [1899, 1900, 1901, 1999, 2000, 2001, 2023, 2024, 2025, 2099, 2100, 2101, 2399, 2400];
for (var yi = 0; yi < years.length; yi++) {
  var d = new Date(Date.UTC(years[yi], 1, 28, 12, 0, 0));
  d.setDate(29); // may roll to Mar 1 on non-leap
  for (var step = 0; step < 20; step++) {
    var y2 = years[(yi + step) % years.length];
    d.setFullYear(y2);
    if (step % 3 === 0) d.setMonth(1); // force Feb
    if (step % 5 === 0) d.setDate(29);
    var f = new Date(d.getTime());
    var l1 = d.getFullYear() + "," + d.getMonth() + "," + d.getDate() + "," + d.getHours() + "," + d.getDay();
    var l2 = f.getFullYear() + "," + f.getMonth() + "," + f.getDate() + "," + f.getHours() + "," + f.getDay();
    var row = y2 + ":" + step + ":" + l1 + ":" + d.getTime();
    feed += row + "\n";
    // sticky: an LMT-era row can leak sub-minute seconds into later ops via
    // fields that setFullYear preserves, so everything after the first
    // pre-1972 row stays in the [LMT] bucket
    if (d.getFullYear() < 1972) sticky = true;
    if (sticky) { feedOld += row + "\n"; if ((yi * 20 + step) % 61 === 0) __L(0, "[LMT] ROW " + row); }
    else { feedModern += row + "\n"; if ((yi * 20 + step) % 61 === 0) __L(1, "ROW " + row); }
    if (l1 !== l2) { mism++; if (mism < 6) __L(2, "MISMATCH y=" + y2 + " s=" + step + " " + l1 + " vs " + l2); }
  }
}
__L(3, "SUMMARY mism=" + mism + " hashModern=" + fnv(feedModern));
__L(4, "[LMT] hashAll=" + fnv(feed) + " hashOld=" + fnv(feedOld));

summary("builtins_ext");
