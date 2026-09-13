// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["U+7D len=1 parts=1 cc=125 join=true", "U+7E len=1 parts=1 cc=126 join=true", "U+7F len=1 parts=1 cc=127 join=true", "U+80 len=1 parts=1 cc=128 join=true", "U+81 len=1 parts=1 cc=129 join=true", "U+FE len=1 parts=1 cc=254 join=true", "U+FF len=1 parts=1 cc=255 join=true", "U+100 len=1 parts=1 cc=256 join=true", "U+101 len=1 parts=1 cc=257 join=true", "U+7FF len=1 parts=1 cc=2047 join=true", "U+800 len=1 parts=1 cc=2048 join=true", "U+D7FF len=1 parts=1 cc=55295 join=true", "U+D800 len=1 parts=1 cc=55296 join=true", "U+DBFF len=1 parts=1 cc=56319 join=true", "U+DC00 len=1 parts=1 cc=56320 join=true", "U+DFFF len=1 parts=1 cc=57343 join=true", "U+E000 len=1 parts=1 cc=57344 join=true", "U+FFFF len=1 parts=1 cc=65535 join=true"];
__EXP[1] = ["mix n=6 codes=126,127,128,255,256,257"];
__EXP[2] = ["mixJoin=true"];
__EXP[3] = ["astral parts=2 c0=d83d c1=de00"];
__EXP[4] = ["empty parts=0"];
__EXP[5] = ["hash=fae18278"];
// test_str_split_boundary_chars.js — split("") across the latin1/wide code
// point transitions: 0x7E/0x7F/0x80/0xFF/0x100/0x101, lone surrogates, astral
// pair. Every char's charCodeAt + array length + join round-trip must match node.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var cps = [0x7D, 0x7E, 0x7F, 0x80, 0x81, 0xFE, 0xFF, 0x100, 0x101, 0x7FF,
           0x800, 0xD7FF, 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0xE000, 0xFFFF];
for (var i = 0; i < cps.length; i++) {
  var s = String.fromCharCode(cps[i]);
  var a = s.split("");
  var line = "U+" + cps[i].toString(16).toUpperCase() + " len=" + s.length +
    " parts=" + a.length + " cc=" + a[0].charCodeAt(0) + " join=" + (a.join("") === s);
  feed += line + "\n";
  __L(0, line);
}
// mixed string across the boundaries
var mix = String.fromCharCode(0x7E, 0x7F, 0x80, 0xFF, 0x100, 0x101);
var am = mix.split("");
feed += "mix n=" + am.length + " codes=" + am.map(function(c){return c.charCodeAt(0);}).join(",") + "\n";
__L(1, "mix n=" + am.length + " codes=" + am.map(function(c){return c.charCodeAt(0);}).join(","));
__L(2, "mixJoin=" + (am.join("") === mix));
feed += "mixJoin=" + (am.join("") === mix) + "\n";
// astral: split("") yields surrogate HALVES (spec: split by UTF-16 code unit)
var as = String.fromCharCode(0xD83D, 0xDE00); // U+1F600
var aa = as.split("");
__L(3, "astral parts=" + aa.length + " c0=" + aa[0].charCodeAt(0).toString(16) + " c1=" + aa[1].charCodeAt(0).toString(16));
feed += "astral parts=" + aa.length + "\n";
// empty string
__L(4, "empty parts=" + "".split("").length);
feed += "empty\n";
__L(5, "hash=" + fnv(feed));

summary("builtins_ext");
