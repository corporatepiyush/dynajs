// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["fails=0 hash=6700166e"];
// test_norm_nfd_nfc_chains.js — NFD->NFC composition chains and idempotence:
// nfc(nfd(nfc(x))) === nfc(x), nfd(nfd(x)) === nfd(x), nfc(nfc(x)) === nfc(x),
// nfd then NFC round-trips, on 200 seeded mixed scripts. Byte-identical vs node.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0xC0A1);
var fails = 0, feed = "";
for (var i = 0; i < 200; i++) {
  // build deliberately messy strings: mixed precomposed + decomposed + marks
  var s = "";
  var n = 1 + (r() % 20);
  for (var k = 0; k < n; k++) {
    var pick = r() % 5;
    if (pick === 0) s += "e\u0301";                 // decomposed
    else if (pick === 1) s += "\u00e9";             // precomposed
    else if (pick === 2) s += String.fromCharCode(0x0300 + (r() % 0x20)); // bare combining mark
    else if (pick === 3) s += String.fromCharCode(0x1100 + (r() % 0x40)); // Hangul jamo
    else s += String.fromCharCode(0x41 + (r() % 26));
  }
  var nfc = s.normalize("NFC");
  var nfd = s.normalize("NFD");
  if (nfc.normalize("NFC") !== nfc) fails++;
  if (nfd.normalize("NFD") !== nfd) fails++;
  if (nfd.normalize("NFC") !== nfc) fails++;
  if (nfc.normalize("NFD") !== nfd) fails++;
  if (nfc.normalize("NFD").normalize("NFC") !== nfc) fails++;
  if (s.normalize("NFC").normalize("NFC") !== s.normalize("NFC")) fails++;
  feed += i + ":" + fnv(nfc) + ":" + fnv(nfd) + ":" + s.length + ":" + nfc.length + ":" + nfd.length + "\n";
}
__L(0, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
