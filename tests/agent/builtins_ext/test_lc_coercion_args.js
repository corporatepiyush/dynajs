// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["[SIGN] undef=SIGN:zero"];
__EXP[1] = ["[SIGN] null=SIGN:zero"];
__EXP[2] = ["[SIGN] num=SIGN:zero neg=SIGN:zero"];
__EXP[3] = ["[SIGN] bool=SIGN:zero"];
__EXP[4] = ["[SIGN] obj=SIGN:zero"];
__EXP[5] = [["[SIGN] objValueOf=SIGN:neg", "[SIGN] objValueOf=SIGN:pos"]];
__EXP[6] = ["throw=THROW:RangeError"];
__EXP[7] = ["[SIGN] selfNull=THROW:TypeError"];
__EXP[8] = ["[SIGN] recvNum=THROW:TypeError"];
__EXP[9] = ["hash=f97a33d9"];
__REQ = {"dynajs": {"0": 1, "1": 1, "2": 1, "3": 1, "4": 1, "5": 1, "6": 1, "7": 1, "8": 1, "9": 1}, "node": {"0": 1, "1": 1, "2": 1, "3": 1, "4": 1, "5": 1, "6": 1, "7": 1, "8": 1, "9": 1}};
// test_lc_coercion_args.js — localeCompare argument coercion: undefined,
// null, numbers, objects with toString/valueOf (incl. throwing) — the
// ToString of the argument is spec'd; byte-identical vs node.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
function cmpv(a, b) {
  try {
    var v = a.localeCompare(b);
    return "SIGN:" + (v < 0 ? "neg" : v > 0 ? "pos" : "zero");
  } catch (e) { return "THROW:" + e.constructor.name; }
}
__L(0, "[SIGN] undef=" + cmpv("undefined", undefined));
feed += "undef\n";
__L(1, "[SIGN] null=" + cmpv("null", null));
feed += "null\n";
__L(2, "[SIGN] num=" + cmpv("42", 42) + " neg=" + cmpv("-1", -1));
feed += "num\n";
__L(3, "[SIGN] bool=" + cmpv("true", true));
feed += "bool\n";
__L(4, "[SIGN] obj=" + cmpv("abc", { toString: function () { return "abc"; } }));
feed += "obj\n";
__L(5, "[SIGN] objValueOf=" + cmpv("5", { valueOf: function () { return 5; } }));
feed += "objValueOf\n";
__L(6, "throw=" + cmpv("abc", { toString: function () { throw new RangeError("boom"); } }));
feed += "throw\n";
__L(7, "[SIGN] selfNull=" + cmpv(null, null));
feed += "selfNull\n";
// receiver coercion: (42).localeCompare(..)
__L(8, "[SIGN] recvNum=" + cmpv(42, "42"));
feed += "recvNum\n";
__L(9, "hash=" + fnv(feed));

summary("builtins_ext");
