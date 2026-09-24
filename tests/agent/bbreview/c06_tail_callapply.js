// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = [["ca1-overflow InternalError", "ca1-overflow RangeError"]];
__EXP[3] = [["ca2-overflow InternalError", "ca2-overflow RangeError"]];
__EXP[4] = ["ca3 call-done apply-done"];
__EXP[6] = [["ca4-overflow InternalError", "ca4-overflow RangeError"]];
__EXP[9] = [["ca6-overflow InternalError", "ca6-overflow RangeError"]];
__REQ = {"dynajs": {"1": 1, "3": 1, "4": 1, "6": 1, "9": 1}, "node": {"1": 1, "3": 1, "4": 1, "6": 1, "9": 1}};
// C: .call()/.apply()/bound in return position must NOT be tail (native callee)
function viaCall(n) { if (n === 0) return "call-done"; return viaCall.call(null, n - 1); }
try { __L(0, "ca1", viaCall(200000)); } catch (e) { __L(1, "ca1-overflow", e.constructor.name); }
function viaApply(n) { if (n === 0) return "apply-done"; return viaApply.apply(null, [n - 1]); }
try { __L(2, "ca2", viaApply(200000)); } catch (e) { __L(3, "ca2-overflow", e.constructor.name); }
__L(4, "ca3", viaCall(3), viaApply(3));
const bnd = viaCall.bind(null);
function viaBound(n) { if (n === 0) return "bind-done"; return bnd(n - 1); }
try { __L(5, "ca4", viaBound(200000)); } catch (e) { __L(6, "ca4-overflow", e.constructor.name); }
__A("c06_tail_callapply.js:ca5", function () { assert_eq(viaBound(3), "call-done", "ca5"); });
// reflect.apply
function viaReflect(n) { if (n === 0) return "ref-done"; return Reflect.apply(viaReflect, null, [n - 1]); }
try { __L(8, "ca6", viaReflect(200000)); } catch (e) { __L(9, "ca6-overflow", e.constructor.name); }
__A("c06_tail_callapply.js:ca7", function () { assert_eq(viaReflect(3), "ref-done", "ca7"); });

summary("bbreview");
