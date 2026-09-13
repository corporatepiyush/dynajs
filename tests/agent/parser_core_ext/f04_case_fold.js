// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["p two four ab 48 d"];
__EXP[1] = ["fold-disc"];
__EXP[3] = ["fold-str"];
__EXP[5] = ["nan d one d"];
__EXP[6] = ["big imin imax 1g pow32 d"];
__EXP[7] = ["tc num str d"];
// F4: folded constants as case labels; switch on a folded discriminant;
// folded string cases; folded NaN case matches nothing.
function pick(x) {
  switch (x) {
    case 1 + 1: return "two";
    case 2 + 2: return "four";
    case "a" + "b": return "ab";
    case (3 << 4) + 0 * 9: return "48";
    default: return "d";
  }
}
__L(0, "p", pick(2), pick(4), pick("ab"), pick(48), pick(9));
switch (2 + 3) { case 5: __L(1, "fold-disc"); break; default: __L(2, "bad"); }
switch ("x" + "y") { case "xy": __L(3, "fold-str"); break; default: __L(4, "bad2"); }
function nanCase(x) { switch (x) { case 0 / 0: return "nan"; case 1: return "one"; } return "d"; }
__L(5, "nan", nanCase(NaN), nanCase(1), nanCase(0 / 0));
// folded negative + big constants as labels
function bigCase(x) {
  switch (x) {
    case -2147483648: return "imin";
    case 2147483647: return "imax";
    case 1 << 30: return "1g";
    case 4294967296: return "pow32";
    default: return "d";
  }
}
__L(6, "big", bigCase(-2147483648), bigCase(2147483647), bigCase(1073741824), bigCase(4294967296), bigCase(-0));
// folded case label of a DIFFERENT type than the discriminant (no coercion)
function typeCase(x) { switch (x) { case "1": return "str"; case 1: return "num"; } return "d"; }
__L(7, "tc", typeCase(1), typeCase("1"), typeCase(true));

summary("parser_core_ext");
