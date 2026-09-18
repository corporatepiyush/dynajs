// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["use-in-block-before"];
__EXP[3] = ["1"];
{ __L(0, "use-in-block-before"); try { __L(1, OPX.HALT); } catch (e) { __A("rp22_tdz_block.js:threw", function () { assert_eq(e.constructor.name, "ReferenceError", "threw"); }); } }
const OPX = { HALT: 1 };
__L(3, OPX.HALT);

summary("constprop_review");
