// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["w1d w-done"];
__EXP[2] = ["w1d-overflow RangeError"];
__EXP[4] = ["w2d w2-done"];
__EXP[5] = ["w2d-overflow RangeError"];
__EXP[10] = ["w4d w4-done"];
__EXP[11] = ["w4d-overflow RangeError"];
__REQ = {"dynajs": {"1": 1, "4": 1, "10": 1}, "node": {"2": 1, "5": 1, "11": 1}};
// C: tail calls × with-statement
const o1 = { v: 1 };
function w1(n, obj) {
  with (obj) { if (n === 0) return "w-done"; }
  return w1(n - 1, obj);
}
__A("c02_tail_with.js:w1s", function () { assert_eq(w1(5, o1), "w-done", "w1s"); });
try { __L(1, "w1d", w1(200000, o1)); } catch (e) { __L(2, "w1d-overflow", e.constructor.name); }
function w2(n, obj) {
  with (obj) {
    if (n === 0) return "w2-done";
    return w2(n - 1, obj);
  }
}
__A("c02_tail_with.js:w2s", function () { assert_eq(w2(5, o1), "w2-done", "w2s"); });
try { __L(4, "w2d", w2(200000, o1)); } catch (e) { __L(5, "w2d-overflow", e.constructor.name); }
// with + catch interplay: throw at bottom of a NON-tail chain, caught in with+catch
function w3(n, obj) {
  with (obj) {
    try {
      if (n === 0) throw new Error("w3-boom");
      w3(n - 1, obj);            // expression statement — NOT a tail call
    } catch (e) {
      return "w3-caught@" + (n > 100 ? "deep" : n);
    }
  }
}
__A("c02_tail_with.js:w3s", function () { assert_eq(w3(5, o1), undefined, "w3s"); });
try { __A("c02_tail_with.js:w3d", function () { assert_eq(w3(100000, o1), undefined, "w3d"); }); } catch (e) { __L(8, "w3d-overflow", e.constructor.name); }
// with over a scope object, tail after it (both branches terminate)
const scope = { a: 10, b: 20 };
function w4(n) {
  with (scope) { var t = a + b; }
  if (n === 0) return "w4-done";
  return w4(n - 1);
}
__A("c02_tail_with.js:w4s", function () { assert_eq(w4(3), "w4-done", "w4s"); });
try { __L(10, "w4d", w4(200000)); } catch (e) { __L(11, "w4d-overflow", e.constructor.name); }

summary("bbreview");
