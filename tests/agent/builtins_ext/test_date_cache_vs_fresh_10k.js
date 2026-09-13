// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ROW 0 5804306457191706 185901,1,25,2,43,11,706,1,-330 | 185901,1,25,2,43,11,706,1,-330", "ROW 1493 2948512798100710 95404,7,26,18,18,20,710,0,-330 | 95404,7,26,18,18,20,710,0,-330", ["[LMT] ROW 2986 -4577126209437847 -143074,7,24,10,49,2,153,6,-353 | -143074,7,24,10,49,2,153,6,-353", "[LMT] ROW 2986 -4577126209437847 -143074,7,24,10,49,30,153,6,-353 | -143074,7,24,10,49,30,153,6,-353"], ["[LMT] ROW 4479 -6884682029485703 -216197,1,11,11,1,34,297,5,-353 | -216197,1,11,11,1,34,297,5,-353", "[LMT] ROW 4479 -6884682029485703 -216197,1,11,11,2,2,297,5,-353 | -216197,1,11,11,2,2,297,5,-353"], "ROW 5972 2222692067921161 72404,3,22,20,48,41,161,4,-330 | 72404,3,22,20,48,41,161,4,-330", "ROW 7465 6262904428392649 200433,6,17,0,3,12,649,0,-330 | 200433,6,17,0,3,12,649,0,-330", ["[LMT] ROW 8958 -2401417113393545 -74128,1,14,16,36,26,455,3,-353 | -74128,1,14,16,36,26,455,3,-353", "[LMT] ROW 8958 -2401417113393545 -74128,1,14,16,36,54,455,3,-353 | -74128,1,14,16,36,54,455,3,-353"]];
__EXP[3] = ["SUMMARY mism=0 hashModern=1f6eaf42"];
__EXP[4] = [["[LMT] hashAll=3b9323bf hashPre1972=636f6c0f", "[LMT] hashAll=665b7f79 hashPre1972=ab5886e1"]];
__REQ = {"dynajs": {"1": 7, "3": 1, "4": 1}, "node": {"1": 7, "3": 1, "4": 1}};
// test_date_cache_vs_fresh_10k.js — per-object breakdown cache vs fresh object
// 10k seeded random epochs across the full valid range (±8.64e15). For each:
// cached-repeat reads on one object must equal fresh-object reads. Also
// exercises the setTime construction path. Sampled rows printed for eyeballing.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0xD1B0CAFE);
var feed = "", mism = 0;
var feedModern = ""; // rows whose local year >= 1900 (byte-oracle vs node)
var feedOld = "";    // pre-1900 local rows: documented LMT divergence class
for (var i = 0; i < 10000; i++) {
  var t = Math.floor((r() / 4294967296 * 2 - 1) * 8.64e15);
  var a = new Date(t);
  var b = new Date(t);
  var c = new Date(); c.setTime(t);
  function g9(d) {
    return d.getFullYear() + "," + d.getMonth() + "," + d.getDate() + "," +
           d.getHours() + "," + d.getMinutes() + "," + d.getSeconds() + "," +
           d.getMilliseconds() + "," + d.getDay() + "," + d.getTimezoneOffset();
  }
  var ga = g9(a), ga2 = g9(a), gb = g9(b), gc = g9(c);
  var line = i + " " + t + " " + ga + " | " + gb;
  feed += line + "\n";
  if (ga !== ga2 || ga !== gb || ga !== gc) {
    mism++;
    if (mism < 6) __L(0, "MISMATCH " + line + " | " + gc);
  }
  if (new Date(t).getFullYear() >= 1972) feedModern += line + "\n";
  else feedOld += line + "\n";
  if (i % 1493 === 0) {
    var yr = new Date(t).getFullYear();
    __L(1, (yr < 1972 ? "[LMT] ROW " : "ROW ") + line);
  }
  // UTC getters through the same cache paths
  var u1 = a.getUTCFullYear() + "," + a.getUTCMonth() + "," + a.getUTCDate() + "," + a.getUTCHours() + "," + a.getUTCDay();
  var u2 = b.getUTCFullYear() + "," + b.getUTCMonth() + "," + b.getUTCDate() + "," + b.getUTCHours() + "," + b.getUTCDay();
  if (u1 !== u2) { mism++; if (mism < 6) __L(2, "UMISMATCH " + i + " " + u1 + " vs " + u2); }
  feed += u1 + "\n";
}
__L(3, "SUMMARY mism=" + mism + " hashModern=" + fnv(feedModern));
__L(4, "[LMT] hashAll=" + fnv(feed) + " hashPre1972=" + fnv(feedOld));

summary("builtins_ext");
