// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// arrows have no own `arguments`: fold through arrow is CORRECT here
const arguments = Object.freeze({ A: 1 });
const f = () => arguments.A;
__A("rp66_arrow_arguments.js:arrow", function () { assert_eq(f(), 1, "arrow"); });

summary("constprop_review");
