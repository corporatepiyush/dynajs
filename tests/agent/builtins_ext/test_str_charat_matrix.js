// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["latin1 len=5 | @-2: cA=NaN cC=NaN at=255 cp=undefined br=undef | @-1: cA=NaN cC=NaN at=122 cp=undefined br=undef | @0: cA=97 cC=97 at=97 cp=97 br=97 | @1: cA=127 cC=127 at=127 cp=127 br=127 | @2: cA=128 cC=128 at=128 cp=128 br=128 | @3: cA=255 cC=255 at=255 cp=255 br=255 | @4: cA=122 cC=122 at=122 cp=122 br=122 | @5: cA=NaN cC=NaN at=undef cp=undefined br=undef | @6: cA=NaN cC=NaN at=undef cp=undefined br=undef | @1e+21: cA=NaN cC=NaN at=undef cp=undefined br=undef | @0: cA=97 cC=97 at=97 cp=97 br=97", "wide len=4 | @-2: cA=NaN cC=NaN at=19990 cp=undefined br=undef | @-1: cA=NaN cC=NaN at=122 cp=undefined br=undef | @0: cA=97 cC=97 at=97 cp=97 br=97 | @1: cA=256 cC=256 at=256 cp=256 br=256 | @2: cA=19990 cC=19990 at=19990 cp=19990 br=19990 | @3: cA=122 cC=122 at=122 cp=122 br=122 | @3: cA=122 cC=122 at=122 cp=122 br=122 | @4: cA=NaN cC=NaN at=undef cp=undefined br=undef | @5: cA=NaN cC=NaN at=undef cp=undefined br=undef | @1e+21: cA=NaN cC=NaN at=undef cp=undefined br=undef | @0: cA=97 cC=97 at=97 cp=97 br=97", "astral len=4 | @-2: cA=NaN cC=NaN at=56832 cp=undefined br=undef | @-1: cA=NaN cC=NaN at=98 cp=undefined br=undef | @0: cA=97 cC=97 at=97 cp=97 br=97 | @1: cA=55357 cC=55357 at=55357 cp=128512 br=55357 | @2: cA=56832 cC=56832 at=56832 cp=56832 br=56832 | @3: cA=98 cC=98 at=98 cp=98 br=98 | @3: cA=98 cC=98 at=98 cp=98 br=98 | @4: cA=NaN cC=NaN at=undef cp=undefined br=undef | @5: cA=NaN cC=NaN at=undef cp=undefined br=undef | @1e+21: cA=NaN cC=NaN at=undef cp=undefined br=undef | @0: cA=97 cC=97 at=97 cp=97 br=97"];
__EXP[1] = ["negzero charAt=a at=a br=a"];
__EXP[2] = ["nan charAt=[a] cc=97"];
__EXP[3] = ["hash=4658ce94"];
// test_str_charat_matrix.js — charAt/charCodeAt/at/codePointAt/bracket index
// reads at boundaries: 0, last, OOB, negative, on latin1/wide/astral strings.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var strs = { latin1: "a\u007f\u0080\u00ffz", wide: "a\u0100\u4e16z", astral: "a\uD83D\uDE00b" };
for (var name in strs) {
  var s = strs[name];
  var row = name + " len=" + s.length;
  var idxs = [-2, -1, 0, 1, 2, 3, s.length - 1, s.length, s.length + 1, 1e21, -0];
  for (var j = 0; j < idxs.length; j++) {
    var ix = idxs[j];
    // print code units, not raw chars (lone-surrogate terminal encoding is impl-defined)
    row += " | @" + ix + ":" +
      " cA=" + s.charAt(ix).charCodeAt(0) +
      " cC=" + s.charCodeAt(ix) +
      " at=" + (s.at(ix) === undefined ? "undef" : s.at(ix).charCodeAt(0)) +
      " cp=" + s.codePointAt(ix) +
      " br=" + (s[ix] === undefined ? "undef" : s[ix].charCodeAt(0));
  }
  feed += row + "\n";
  __L(0, row);
}
// -0 must behave as 0
__L(1, "negzero charAt=" + "abc".charAt(-0) + " at=" + "abc".at(-0) + " br=" + "abc"[-0]);
feed += "negzero\n";
// charCodeAt of NaN index
__L(2, "nan charAt=[" + "abc".charAt(NaN) + "] cc=" + "abc".charCodeAt(NaN));
feed += "nan\n";
__L(3, "hash=" + fnv(feed));

summary("builtins_ext");
