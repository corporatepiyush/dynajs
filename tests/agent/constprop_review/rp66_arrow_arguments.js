__EXP = null;
const arguments = Object.freeze({ A: 1 });
const f = () => arguments.A;
__A("rp66_arrow_arguments.js:arrow", function () { assert_eq(f(), 1, "arrow"); });

summary("constprop_review");
