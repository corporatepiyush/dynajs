// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["ss1 sl-done s-done"];
__EXP[1] = ["ss2 s-done"];
__EXP[2] = ["ss2-overflow RangeError"];
__EXP[3] = ["ss3 m-done m-done"];
__EXP[4] = ["ss3-overflow RangeError"];
__EXP[6] = ["ss4b object"];
__EXP[7] = ["ss4b-overflow RangeError"];
__EXP[10] = [["ss6-overflow InternalError", "ss6-overflow RangeError"]];
__EXP[11] = ["ss7a sa:1 sa-end"];
__EXP[12] = ["ss7b sa-end"];
__EXP[13] = ["ss7b-overflow RangeError"];
__REQ = {"dynajs": {"0": 1, "1": 1, "3": 1, "6": 1, "10": 1, "11": 1, "12": 1}, "node": {"0": 1, "2": 1, "4": 1, "7": 1, "10": 1, "11": 1, "13": 1}};
// C: strict <-> sloppy tail calls
function strictRec(n) {
  "use strict";
  if (n === 0) return "s-done";
  return sloppyRec(n - 1);
}
function sloppyRec(n) {
  if (n === 0) return "sl-done";
  return strictRec(n - 1);
}
__L(0, "ss1", strictRec(5), sloppyRec(5));
try { __L(1, "ss2", strictRec(300000)); } catch (e) { __L(2, "ss2-overflow", e.constructor.name); }
const obj = {
  m(n) { "use strict"; if (n === 0) return "m-done"; return this.m2(n - 1); },
  m2(n) { if (n === 0) return "m2-done"; return this.m(n - 1); }
};
try { __L(3, "ss3", obj.m(100000), obj.m2(5)); } catch (e) { __L(4, "ss3-overflow", e.constructor.name); }
function tG(n) { if (n === 0) return typeof this; return tG(n - 1); }
__A("c03_tail_strict.js:ss4a", function () { assert_eq(tG(3), "object", "ss4a"); });
try { __L(6, "ss4b", tG(200000)); } catch (e) { __L(7, "ss4b-overflow", e.constructor.name); }
const arrow = (n) => n === 0 ? "arr-done" : arrow(n - 1);
__A("c03_tail_strict.js:ss5", function () { assert_eq(arrow(5), "arr-done", "ss5"); });
try { __L(9, "ss6", arrow(300000)); } catch (e) { __L(10, "ss6-overflow", e.constructor.name); }
// sloppy->strict mutual tail passing arguments.length (both sides terminate)
function sloppyArgs(n) {
  if (n <= 0) return "sa-end";
  return strictTail(n - 1, arguments.length);
}
function strictTail(n, len) {
  "use strict";
  if (n === 0) return "sa:" + len;
  return sloppyArgs(n - 1);
}
__L(11, "ss7a", sloppyArgs(5), sloppyArgs(4));
try { __L(12, "ss7b", sloppyArgs(100000)); } catch (e) { __L(13, "ss7b-overflow", e.constructor.name); }

summary("bbreview");
