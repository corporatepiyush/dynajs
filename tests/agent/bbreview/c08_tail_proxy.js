// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = [["px1-overflow InternalError", "px1-overflow RangeError"]];
__EXP[3] = ["px3 m-done"];
__EXP[4] = ["px3-overflow RangeError"];
__EXP[6] = ["px5 m-done"];
__EXP[7] = ["px5-overflow RangeError"];
__REQ = {"dynajs": {"1": 1, "3": 1, "6": 1}, "node": {"1": 1, "4": 1, "7": 1}};
// C: tail recursion through Proxy (fallback path)
const handler = { apply(t, thisArg, args) { return t.apply(thisArg, args); } };
const pfn = new Proxy(function rec(n) { if (n === 0) return "proxy-done"; return pfn(n - 1); }, handler);
try { __L(0, "px1", pfn(100000)); } catch (e) { __L(1, "px1-overflow", e.constructor.name); }
const target = { m(n) { if (n === 0) return "m-done"; return this.m(n - 1); } };
const pobj = new Proxy(target, {});
__A("c08_tail_proxy.js:px2", function () { assert_eq(pobj.m(5), "m-done", "px2"); });
try { __L(3, "px3", pobj.m(200000)); } catch (e) { __L(4, "px3-overflow", e.constructor.name); }
// proxy get handler on method lookups
const pobj2 = new Proxy(target, { get(t, k, r) { return Reflect.get(t, k, r); } });
__A("c08_tail_proxy.js:px4", function () { assert_eq(pobj2.m(5), "m-done", "px4"); });
try { __L(6, "px5", pobj2.m(100000)); } catch (e) { __L(7, "px5-overflow", e.constructor.name); }

summary("bbreview");
