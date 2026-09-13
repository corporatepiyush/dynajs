// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0 hash=74ddc9f0"];
// test_norm_already_nfc.js — already-NFC wide strings through all 4 forms:
// the "already normalized" fast path must return identical text, byte-identical
// to node, including length preservation (no reallocation side effects).
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x0F4C);
var feed = "";
var fails = 0;
// 100 precomposed NFC strings (Hangul syllables, Latin-1 accented, CJK)
for (var i = 0; i < 100; i++) {
  var s = "";
  var n = 1 + (r() % 30);
  for (var k = 0; k < n; k++) {
    var pick = r() % 3;
    if (pick === 0) s += String.fromCharCode(0xac00 + (r() % 0x2b00)); // Hangul precomposed
    else if (pick === 1) s += String.fromCharCode(0xc0 + (r() % 0x100));
    else s += String.fromCharCode(0x4e00 + (r() % 0x1000));
  }
  var nfc = s.normalize("NFC");
  if (nfc !== s) { fails++; __L(0, "NOT-NFC i=" + i); }
  if (s.normalize("NFD") === s && s !== s.normalize("NFC")) { /* decomposable precomposed: NFD differs — fine */ }
  feed += i + ":" + fnv(s.normalize("NFC")) + ":" + fnv(s.normalize("NFD")) + ":" +
          fnv(s.normalize("NFKC")) + ":" + fnv(s.normalize("NFKD")) + ":" + s.length + "\n";
}
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
