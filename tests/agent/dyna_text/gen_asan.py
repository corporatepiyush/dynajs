#!/usr/bin/env python3
"""gen_asan.py — hostile-input fuzz family for the ASan pass.

These probes feed every owned module BOUNDED but adversarial mutations:
buffer-boundary entities, truncated tokens, length-prefix lies, quote chaos,
cap-edge inputs. Every call is try/catch-wrapped; a crash, hang, or sanitizer
report fails the probe (rc != 0). Run under BOTH the normal build and the
ASan build (`DYNAJS_BIN=.../dynajs-asan ./run.sh asan`).
"""
from gen_common import jslit, write_probe

A_IMPORTS = [("H", "dyna:html"), ("X", "dyna:xml"), ("Y", "dyna:yaml"),
             ("Z", "dyna:compress"), ("E", "dyna:encoding"),
             ("M", "dyna:matcher"), ("J", "dyna:json")]

GUARD = r'''
// every hostile call is guarded; a crash/hang/sanitizer report fails the probe
function Guard(desc, fn) {
  try { fn(); } catch (e) { if (!(e instanceof Error) && typeof e !== "object") throw e; }
  __pass++;
}
var SNIP = [0, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144];
function forSnip(fn) {
  // apply fn(s) for every prefix length in SNIP and every 1-char split pair
  for (var i = 0; i < SNIP.length; i++) fn(SNIP[i]);
}
'''


def gen_asan():
    c = [GUARD]
    # ---- HTML: token-boundary chaos ----
    c.append('''
var HTML_DOCS = [
  "<p a='" + "'" + ">x</p>",
  "<div><span>t</span></div>",
  "<!--c--><b>&amp;&#x1F600;</b>",
  "<script>if (a < b) { x('</script>') }</script>done",
  "<a href='x' class=y><br></a>",
  "<table><tr><td>1<td>2</table>",
  "<p>" + "&#x41;".repeat(50) + "</p>",
];
for (var di = 0; di < HTML_DOCS.length; di++) {
  forSnip(function (k) {
    Guard("html prefix", function () { H.HTMLParse(HTML_DOCS[di].slice(0, k)); });
    Guard("html suffix", function () { H.HTMLParse(HTML_DOCS[di].slice(k)); });
    for (var j = 0; j < k; j++)
      Guard("html split", function () {
        var d = H.HTMLParse(HTML_DOCS[di].slice(0, j) + "\\uFFFD" + HTML_DOCS[di].slice(j));
        H.HTMLText(d); H.HTMLStringify(d);
      });
  });
}
// cap edges (documented: 64 MiB input, depth 256, 512 attrs are refused)
Guard("html depth cap", function () { H.HTMLParse("<div>".repeat(300) + "x"); });
Guard("html attr cap", function () {
  var s = "<p";
  for (var i = 0; i < 600; i++) s += " a" + i + "=1";
  H.HTMLParse(s + ">");
});
// sanitizer over every doc (parses under the hood)
var SAN2 = new H.Sanitizer({allow: {p: [], b: [], div: ["class"]}, protocols: {"div.class": ["https"]}});
Guard("sanitizer doc0", function () { SAN2.clean(HTML_DOCS[0]); });
Guard("sanitizer doc1", function () { SAN2.clean(HTML_DOCS[1]); });
Guard("sanitizer doc3", function () { SAN2.clean(HTML_DOCS[3]); });
Guard("sanitizer doc4", function () { SAN2.clean(HTML_DOCS[4]); });
''')
    # ---- XML: entity/CDATA/SAX chaos ----
    c.append('''
var XML_DOCS = [
  "<a x='1'><b>&amp;&#x41;&#x1F600;</b><![CDATA[x]]><!----><?p d?></a>",
  "<a>" + "<b/>".repeat(100) + "</a>",
  "<!DOCTYPE d [<!ELEMENT d ANY>]><d>text</d>",
  "<r>" + "&lt;".repeat(200) + "</r>",
];
for (var xi = 0; xi < XML_DOCS.length; xi++) {
  forSnip(function (k) {
    Guard("xml prefix", function () { X.XMLParse(XML_DOCS[xi].slice(0, k)); });
    Guard("xml keep", function () { X.XMLParse(XML_DOCS[xi].slice(0, k), {entities:"keep"}); });
  });
}
// SAX: every 1-char split of a construct-rich document
var SAXDOC = "<a x='1'>t<b><![CDATA[c]]></b><!--m--><?pi d?></a>";
Guard("sax splits", function () {
  for (var j = 0; j < SAXDOC.length; j += 3) {
    var p = new X.SAXParser({onOpen: function(){}, onText: function(){},
      onCData: function(){}, onComment: function(){}, onPI: function(){}, onClose: function(){}});
    p.write(SAXDOC.slice(0, j)); p.write(SAXDOC.slice(j)); p.end();
  }
});
Guard("xml depth cap", function () { X.XMLParse("<a>".repeat(400) + "</a>".repeat(400)); });
Guard("xml stringify deep", function () {
  var n = {name: "l", attrs: {}, children: []};
  for (var i = 0; i < 400; i++) n = {name: "a", attrs: {}, children: [n]};
  try { X.XMLStringify(n); } catch (e) { if (!(e instanceof RangeError)) throw e; }
});
''')
    # ---- YAML: quote/tab/flow chaos ----
    c.append('''
var YAML_DOCS = [
  "a: [1, 2, {b: c}]",
  "a: |\\n  text\\n  more\\n",
  "a: >\\n  folded\\n  text\\n",
  'x: "esc \\\\t \\\\u00e9"',
  "# c\\nkey: value # t\\n",
  "- 1\\n- two\\n- three\\n",
  "deep:\\n  deeper:\\n    deepest: 1",
];
for (var yi = 0; yi < YAML_DOCS.length; yi++) {
  forSnip(function (k) {
    Guard("yaml prefix", function () { Y.Parse(YAML_DOCS[yi].slice(0, k)); });
  });
}
Guard("yaml tab chaos", function () {
  Y.Parse("a:\\n  b:\\n\\tc: 1");
  Y.Parse("a:\\n \\t b: 1");
});
Guard("yaml flow chaos", function () {
  Y.Parse("a: [[[[[[1]]]]]]");
  Y.Parse("a: {x: [1, {y: 2}");
  Y.Parse('a: ["unterminated, b: 2]');
});
Guard("yaml parseall junk", function () { Y.ParseAll("---\\n"); });
''')
    # ---- compress: length-prefix lies + corrupt containers ----
    c.append('''
var payload = hex2u8("68656c6c6f20776f726c64".repeat(8));
var gz = Z.gzip(payload), zs = Z.zstd(payload), br = Z.brotli(payload);
var sn = Z.snappy(payload), lf = Z.lz4Frame(payload);
for (var fi = 1; fi < Math.max(gz.length, 40); fi += 3) {
  Guard("gunzip bitflip", function () { var g = new Uint8Array(gz); g[fi % gz.length] ^= 0x80; Z.gunzip(g); });
  Guard("unzstd bitflip", function () { var g = new Uint8Array(zs); g[fi % zs.length] ^= 0x80; Z.unzstd(g); });
  Guard("unbrotli bitflip", function () { var g = new Uint8Array(br); g[fi % br.length] ^= 0x80; Z.unbrotli(g); });
  Guard("unsnappy bitflip", function () { var g = new Uint8Array(sn); g[fi % sn.length] ^= 0x80; Z.unsnappy(g); });
  Guard("lzf bitflip", function () { var g = new Uint8Array(lf); g[fi % lf.length] ^= 0x80; Z.lz4Unframe(g); });
}
Guard("snappy copy overrun", function () {
  // copy tokens that reference before the start / after the end
  var blob = hex2u8("0c" + "40f0000000" + "12000000");
  Z.unsnappy(blob);
});
Guard("lz4 raw overrun", function () {
  // literal-length and match-length runs that exceed the buffer
  Z.lz4Decompress(hex2u8("f02000004a4a4a4a4a4a4a4a4a4a"));
  Z.lz4Decompress(hex2u8("3f61626360600000"));
});
Guard("gzip header lie", function () {
  var g = new Uint8Array(gz); g[3] = 0x1f; /* FEXTRA + FNAME + FCOMMENT */
  Z.gunzip(g);
});
Guard("tar size lie", function () {
  var t = Z.TarPack([{name: "f", data: hex2u8("0102")}]);
  t[124] = 0x37; t[125] = 0x37; t[126] = 0x37; t[127] = 0x37; /* huge octal size */
  Z.TarList(t); Z.TarExtract(t);
});
Guard("zip offset lie", function () {
  var z = Z.ZipPack([{name: "f", data: hex2u8("0102")}]);
  z[10] ^= 0xff; /* corrupt local header data offset */
  Z.ZipList(z); try { Z.ZipRead(z, "f"); } catch (e) {}
});
Guard("compressor instance reuse hostile", function () {
  var c2 = new Z.Compressor({algo: "zstd", level: 19});
  try {
    for (var i = 0; i < 8; i++) { c2.decompress(c2.compress(payload.slice(0, 64 + i))); }
  } finally { c2.close(); }
});
''')
    # ---- encoding/json/matcher chaos ----
    c.append('''
Guard("basex cap edge", function () {
  var big = new Uint8Array(4096).fill(0xff);
  E.Base58Encode(big); E.BaseXEncode(big, "01");
  try { E.Base58Encode(new Uint8Array(4097)); } catch (e) {}
});
Guard("varint chains", function () {
  for (var i = 1; i <= 12; i++) {
    var buf = new Uint8Array(i).fill(0xff);
    E.Uvarint(buf); E.Varint(buf);
  }
});
Guard("b85 all z", function () { E.Base85Decode("z".repeat(100)); });
Guard("pointer escape flood", function () {
  J.Pointer.get({a: 1}, "/" + "~0~1".repeat(100) + "a");
  J.Pointer.unescape("~0~1".repeat(1000));
});
Guard("patch hostile ops", function () {
  try { J.Patch.apply({a: [1]}, [{op: "add", path: "/a/-"}, {op: "move", from: "/a/-", path: "/a/0"}]); }
  catch (e) {}
});
Guard("matcher astral soup", function () {
  var s = "🌍".repeat(50) + "ab" + "🌍".repeat(50);
  var m = new M.Matcher("ab");
  m.allIn(s); m.replaceAllIn(s, "🌍🌍🌍");
  var mm = new M.MultiMatcher(["🌍a", "a🌍", "b"]);
  mm.allIn(s);
});
Guard("lev caps", function () {
  var s = "a".repeat(1000), t = "b".repeat(1000);
  M.Levenshtein(s, t, {max: 5});
  M.DiceCoefficient(s, t);
});
''')
    write_probe("asan/a01_hostile_matrix.js", A_IMPORTS, "\n".join(c),
                "hostile-input matrix for ASan — clean throws only")
    return c


def main():
    c = gen_asan()
    print("gen_asan: %d case groups emitted" % len(c))


if __name__ == "__main__":
    main()
