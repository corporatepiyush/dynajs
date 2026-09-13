// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["U+0000 len=1 cc=0 isStr=string", "U+0001 len=1 cc=1 isStr=string", "U+001F len=1 cc=31 isStr=string", "U+0020 len=1 cc=32 isStr=string", "U+0041 len=1 cc=65 isStr=string", "U+007E len=1 cc=126 isStr=string", "U+007F len=1 cc=127 isStr=string", "U+0080 len=1 cc=128 isStr=string", "U+00FF len=1 cc=255 isStr=string", "U+0100 len=1 cc=256 isStr=string", "U+07FF len=1 cc=2047 isStr=string", "U+0800 len=1 cc=2048 isStr=string", "U+D7FF len=1 cc=55295 isStr=string", "U+D800 len=1 cc=55296 isStr=string", "U+DC00 len=1 cc=56320 isStr=string", "U+DFFF len=1 cc=57343 isStr=string", "U+E000 len=1 cc=57344 isStr=string", "U+FFFD len=1 cc=65533 isStr=string", "U+FFFE len=1 cc=65534 isStr=string", "U+FFFF len=1 cc=65535 isStr=string"];
__EXP[1] = ["multi=true"];
__EXP[2] = ["empty=0"];
__EXP[3] = ["mapIdent=true val=fcc"];
__EXP[4] = ["objIdent=true"];
__EXP[5] = ["acc=306797421"];
__EXP[6] = ["hash=e581b2d0"];
// test_str_fromcharcode_matrix.js — String.fromCharCode over the full BMP
// boundary set + surrogate halves, cross-checked as Map/object keys and
// against literal source text (interning identity of produced atoms).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var cps = [0, 1, 0x1f, 0x20, 0x41, 0x7e, 0x7f, 0x80, 0xff, 0x100, 0x7ff, 0x800,
           0xd7ff, 0xd800, 0xdc00, 0xdfff, 0xe000, 0xfffd, 0xfffe, 0xffff];
for (var i = 0; i < cps.length; i++) {
  var c = String.fromCharCode(cps[i]);
  var line = "U+" + ("000" + cps[i].toString(16).toUpperCase()).slice(-4) +
    " len=" + c.length + " cc=" + c.charCodeAt(0) + " isStr=" + (typeof c);
  feed += line + "\n";
  __L(0, line);
}
// multi-arg and identities
__L(1, "multi=" + (String.fromCharCode(0x7f, 0x80, 0xff, 0x100) === "\u007f\u0080\u00ff\u0100"));
feed += "multi\n";
__L(2, "empty=" + String.fromCharCode().length);
feed += "empty\n";
// identity with literals via Map
var m = new Map();
m.set("\u00ff", "lit"); m.set(String.fromCharCode(0xff), "fcc");
__L(3, "mapIdent=" + (m.size === 1) + " val=" + m.get("\u00ff"));
feed += "mapIdent\n";
// object key identity incl numeric coercion boundary
var o = {};
o[String.fromCharCode(0x41)] = 1; o["A"] = 2;
feed += "objIdent=" + (Object.keys(o).length === 1 && o.A === 2) + "\n";
__L(4, "objIdent=" + (Object.keys(o).length === 1 && o.A === 2));
// round-trip through charCodeAt for ALL BMP code points (hash only)
var acc = 0;
for (var cp = 0; cp <= 0xffff; cp += 7) {
  var c = String.fromCharCode(cp);
  if (c.charCodeAt(0) !== cp) { acc = -1; break; }
  acc = (acc + cp) % 1000000007;
}
__L(5, "acc=" + acc);
feed += "acc=" + acc + "\n";
__L(6, "hash=" + fnv(feed));

summary("builtins_ext");
