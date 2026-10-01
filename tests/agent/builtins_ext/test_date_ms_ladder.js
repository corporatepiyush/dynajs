__EXP = {};
__EXP[2] = ["ROW 1718495996798 798,56,29,5,1718495996798", "ROW 1718495997399 399,57,29,5,1718495997399", "ROW 1718495998000 0,58,29,5,1718495998000", "ROW 1718495998601 601,58,29,5,1718495998601", "ROW 1718495999202 202,59,29,5,1718495999202"];
__EXP[3] = ["SUMMARY mism=0 hash=dc9e8c00"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var base = Date.UTC(2024, 5, 15, 23, 59, 58, 0);
var feed = "", mism = 0;
for (var ms = -1500; ms <= 1500; ms++) {
  var a = new Date(base + ms);
  var fa = new Date(base + ms);
  var b = new Date(base); b.setMilliseconds(ms);
  var c = new Date(base + 999); c.setMilliseconds(ms + 1000 - 1000);
  var l = (base + ms) + " " + a.getMilliseconds() + "," + a.getSeconds() + "," + a.getMinutes() + "," + a.getHours() + "," + a.getTime();
  feed += l + "\n";
  if (a.getTime() !== fa.getTime()) mism++;
  if (b.getTime() !== base + ms) { mism++; if (mism < 6) __L(0, "SETMS-MISMATCH ms=" + ms + " got=" + b.getTime() + " want=" + (base + ms)); }
  var e = new Date(base); e.setMilliseconds(ms % 1000);
  if (e.getTime() !== base + (ms % 1000)) { mism++; if (mism < 6) __L(1, "MOD-MISMATCH ms=" + ms); }
  var t2 = new Date(base + ms);
  if (a.getMilliseconds() !== t2.getMilliseconds() || a.getSeconds() !== t2.getSeconds()) mism++;
  if (ms % 601 === 0) __L(2, "ROW " + l);
}
__L(3, "SUMMARY mism=" + mism + " hash=" + fnv(feed));

summary("builtins_ext");
