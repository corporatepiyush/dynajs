// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["b2a 1 0 0"];
__EXP[1] = ["b2b 2 0 0"];
__EXP[2] = ["b2c 2 1"];
// commit 4: fused switch probes — `if (s === "lit")` and `if (obj.prop === x)` shapes
const FROZEN = Object.freeze({ mode: "run", tag: "t1" });
function check1(s) { if (s === "go") return 1; return 0; }
function check2(o) { if (o.mode === "run") return 2; return 0; }
function check3(o) { if (o.tag === FROZEN.tag) return 3; return 0; }
__L(0, "b2a", check1("go"), check1("no"), check1(1));
__L(1, "b2b", check2(FROZEN), check2({ mode: "stop" }), check2({}));
let side = 0;
const gobj = { get mode() { side++; return "run"; } };
__L(2, "b2c", check2(gobj), side);
const p = new Proxy({ mode: "run" }, { get(t, k) { return t[k] + "!"; } });
__A("s01_bank2_fused.js:b2d", function () { assert_eq(check2(p), 0, "b2d"); });
globalThis.gv = "go";
function check4() { if (gv === "go") return 4; return 0; }
__A("s01_bank2_fused.js:b2e", function () { assert_eq(check4(), 4, "b2e"); });
function check5() { if (neverDeclared === "x") return 5; return 0; }
try { check5(); __L(5, "b2f", "no-throw"); } catch (e) { __A("s01_bank2_fused.js:b2f", function () { assert_eq(e.constructor.name, "ReferenceError", "b2f"); }); }
let hits = 0;
for (let i = 0; i < 100000; i++) { const s = i % 2 ? "aa" : "bb"; if (s === "aa") hits++; }
__A("s01_bank2_fused.js:b2g", function () { assert_eq(hits, 50000, "b2g"); });
// prototype-chain property with getter on Object.prototype
Object.defineProperty(Object.prototype, "pmode", { get() { return "run"; }, configurable: true });
try { __A("s01_bank2_fused.js:b2h", function () { assert_eq(check2({}), 0, "b2h"); }); } finally { delete Object.prototype.pmode; }
__A("s01_bank2_fused.js:b2i", function () { assert_eq(typeof neverDeclared, "undefined", "b2i"); });

summary("bbreview");
