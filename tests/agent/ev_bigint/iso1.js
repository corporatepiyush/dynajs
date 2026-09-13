// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
const A = 1n << 524287n;
const B = (1n << 524287n) - 1n;
const MAXV = B << 524287n;
const half = MAXV >> (64n * 8192n);
const p = half * half;
__A("iso1.js:p limbs", function () { assert_eq(p.toString(16).length, 262143, "p limbs"); });
try { const s = p.toString(10); __A("iso1.js:tostr10 ok", function () { assert_eq(s.length, 315652, "tostr10 ok"); }); } catch (e) { __L(2, "tostr10 THROW", e.constructor.name, e.message); }
try { const s = p.toString(36); __A("iso1.js:tostr36 ok", function () { assert_eq(s.length, 202822, "tostr36 ok"); }); } catch (e) { __L(4, "tostr36 THROW", e.constructor.name, e.message); }

summary("ev_bigint");
