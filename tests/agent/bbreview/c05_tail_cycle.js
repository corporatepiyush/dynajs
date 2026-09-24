// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["cycle-small f4 f4"];
__EXP[1] = ["~cycle-deep"];
__EXP[2] = ["~cycle-deep-overflow"];
__EXP[4] = ["~vary-deep"];
__EXP[5] = ["vary-deep-overflow RangeError"];
__EXP[7] = ["wide-deep p1-done"];
__EXP[8] = ["wide-deep-overflow RangeError"];
__REQ = {"dynajs": {"0": 1, "1": 1, "4": 1, "7": 1}, "node": {"0": 1, "2": 1, "5": 1, "8": 1}};
// C: 4-function cycle, 1M total calls — stack must stay flat
let steps = 0;
function f1(n) { steps++; if (n === 0) return "f1"; return f2(n - 1); }
function f2(n) { steps++; if (n === 0) return "f2"; return f3(n - 1); }
function f3(n) { steps++; if (n === 0) return "f3"; return f4(n - 1); }
function f4(n) { steps++; if (n === 0) return "f4"; return f1(n - 1); }
__L(0, "cycle-small", f1(3), f1(3));
steps = 0;
try { __L(1, "cycle-deep", f1(999999), steps); } catch (e) { __L(2, "cycle-deep-overflow", e.constructor.name, steps); }
// argument-count CHANGING cycle (trampoline capacity + arg padding)
function g1(n, a) { if (n === 0) return a; return g2(n - 1, a, n); }
function g2(n, a, b) { if (n === 0) return a + b; return g1(n - 1, a + b); }
__A("c05_tail_cycle.js:vary-small", function () { assert_eq(g1(7, 0), 16, "vary-small"); });
try { __L(4, "vary-deep", g1(200000, 0)); } catch (e) { __L(5, "vary-deep-overflow", e.constructor.name); }
// widening args beyond caller frame
function p1(n) { if (n === 0) return "p1-done"; return p2(n - 1, "x", "y", "z", "w"); }
function p2(n, a, b, c, d) { if (n === 0) return a + b + c + d; return p1(n - 1); }
__A("c05_tail_cycle.js:wide-small", function () { assert_eq(p1(3), "xyzw", "wide-small"); });
try { __L(7, "wide-deep", p1(200000)); } catch (e) { __L(8, "wide-deep-overflow", e.constructor.name); }

summary("bbreview");
