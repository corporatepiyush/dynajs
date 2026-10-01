__EXP = {};
__EXP[2] = ["deep A"];
__EXP[3] = ["deep RangeError"];
__EXP[5] = ["~deep2"];
__EXP[6] = ["deep2 RangeError"];
__REQ = {"dynajs": {"2": 1, "5": 1}, "node": {"3": 1, "6": 1}};
function a0(n) { if (n === 0) return "A"; return a1(n - 1, "x"); }
function a1(n, p1) { if (n === 0) return p1 + "B"; return a2(n - 1, p1, "y"); }
function a2(n, p1, p2) { if (n === 0) return p1 + p2 + "C"; return a3(n - 1, p1 + p2, "z"); }
function a3(n, p1, p2) { if (n === 0) return p1 + p2 + "D"; return a4(n - 1, p1 + p2, 7, 8); }
function a4(n, p1, p2, p3, p4) { if (n === 0) return p1 + p2 + p3 + p4 + "E"; return a0(n - 1); }
__A("p05_ptc_cycle5_arity.js:a", function () { assert_eq(a0(3), "xyzD", "a"); });
__A("p05_ptc_cycle5_arity.js:b", function () { assert_eq(a0(13), "xyzD", "b"); });
try { __L(2, "deep", a0(500000)); } catch (e) { __L(3, "deep", e.constructor.name); }
function s0(n, acc) { if (n === 0) return acc; return s1(n - 1, acc + 1); }
function s1(n, acc) { if (n === 0) return acc; return s2(n - 1, acc + 1); }
function s2(n, acc) { if (n === 0) return acc; return s3(n - 1, acc + 1); }
function s3(n, acc) { if (n === 0) return acc; return s4(n - 1, acc + 1); }
function s4(n, acc) { if (n === 0) return acc; return s0(n - 1, acc + 1); }
__A("p05_ptc_cycle5_arity.js:c", function () { assert_eq(s0(7, 0), 7, "c"); });
try { __L(5, "deep2", s0(500000, 0)); } catch (e) { __L(6, "deep2", e.constructor.name); }

summary("parser_core_ext");
