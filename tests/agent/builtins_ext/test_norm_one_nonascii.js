__EXP = {};
__EXP[0] = ["0 NFC=19aef6d4:14 NFD=fe724dc3:15 NFKC=19aef6d4:14 NFKD=fe724dc3:15", "1 NFC=19aef6d4:14 NFD=fe724dc3:15 NFKC=19aef6d4:14 NFKD=fe724dc3:15", "2 NFC=c6e0a977:15 NFD=c6e0a977:15 NFKC=c6e0a977:15 NFKD=c6e0a977:15", "3 NFC=b15f7fbd:11 NFD=3e9f7097:12 NFKC=b15f7fbd:11 NFKD=3e9f7097:12", "4 NFC=1f4d112d:11 NFD=1f4d112d:11 NFKC=1f4d112d:11 NFKD=1f4d112d:11", "5 NFC=dab2cc08:11 NFD=dab2cc08:11 NFKC=e4eb462a:13 NFKD=e4eb462a:13", "6 NFC=90cddd53:11 NFD=38a27a7c:12 NFKC=90cddd53:11 NFKD=38a27a7c:12", "7 NFC=71c94d24:11 NFD=71c94d24:11 NFKC=a1d42518:12 NFKD=a1d42518:12", "8 NFC=2b79315a:13 NFD=174e57e0:14 NFKC=2b79315a:13 NFKD=174e57e0:14"];
__EXP[1] = ["hash=17489709"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var one = [
  "caf\u00e9", "cafe\u0301",
  "\u0391\u0398\u0397\u039d\u0391",
  "\u0391\u0301",
  "\u4e16",
  "\u00bd",
  "a\u0308",
  "\ufb01",
  "x\u0307\u0323y"
];
for (var i = 0; i < one.length; i++) {
  var s = "ASCII[" + one[i] + "]END";
  var row = i;
  for (var f = 0; f < 4; f++) {
    var forms = ["NFC", "NFD", "NFKC", "NFKD"];
    var out = s.normalize(forms[f]);
    row += " " + forms[f] + "=" + fnv(out) + ":" + out.length;
  }
  __L(0, row);
  feed += row + "\n";
}
__L(1, "hash=" + fnv(feed));

summary("builtins_ext");
