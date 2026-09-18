// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = [["b2 RangeError:bottom", "b2 RangeError:Maximum call stack size exceeded"]];
__EXP[2] = [["b3 TypeError:from-catch-depth", "b3 RangeError:Maximum call stack size exceeded"]];
__EXP[4] = [["b5 42", "b5 undefined"]];
__EXP[5] = [["b6 Maximum call stack size exceeded", "b6 Maximum call stack size exceeded"]];
__REQ = {"dynajs": {"1": 1, "2": 1, "4": 1, "5": 1}, "node": {"1": 1, "2": 1, "4": 1, "5": 1}};
// C: tail call to a function that itself throws
function boom(n) { if (n === 3) throw new Error("mid-boom"); return boom(n + 1); }
function caller() { return boom(0); }
try { caller(); } catch (e) { __A("c04_tail_throw.js:b1", function () { assert_eq(e.message, "mid-boom", "b1"); }); }
function chain(n) { if (n === 0) throw new RangeError("bottom"); return chain(n - 1); }
function top() { try { return chain(100000); } catch (e) { return e.constructor.name + ":" + e.message; } }
__L(1, "b2", top());
function h(n) { if (n === 0) throw new TypeError("from-catch-depth"); return h(n - 1); }
function c2() {
  try { throw new Error("init"); } catch (e) { return h(100000); }
}
try { c2(); } catch (e) { __L(2, "b3", e.constructor.name + ":" + e.message); }
__A("c04_tail_throw.js:b4", function () { assert_eq([1, 2, 3].map(x => x * 2).join(","), "2,4,6", "b4"); });
// throw value identity preserved through deep tail chain
function idThrow(n, v) { if (n === 0) throw v; return idThrow(n - 1, v); }
try { idThrow(100000, { custom: 42 }); } catch (e) { __L(4, "b5", e.custom); }
// rethrow chain
function rethrow(n) {
  try { if (n === 0) throw new Error("r" + n); return rethrow(n - 1); }
  catch (e) { throw e; }
}
try { rethrow(50000); } catch (e) { __L(5, "b6", e.message); }

summary("bbreview");
