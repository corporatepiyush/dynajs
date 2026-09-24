// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["big 100000 0 1 true"];
__EXP[1] = ["chain 100 0 99"];
__EXP[2] = [["~m1 3001", "~m1 3001"]];
__EXP[3] = ["ovr {\"a\":5,\"b\":3,\"c\":4} a,b,c"];
// H: 100k spread + chained spreads + Object.keys over chain results
const big = new Array(100000).fill(7);
big[0] = 0; big[99999] = 1;
const copy = [...big];
__L(0, "big", copy.length, copy[0], copy[99999], Array.isArray(copy));
let o = {};
for (let i = 0; i < 100; i++) o = { ...o, ["k" + i]: i };
__L(1, "chain", Object.keys(o).length, o.k0, o.k99);
const a = { x: 1 }, b = { y: 2 }, c = { z: 3 };
let m = { ...a };
for (let i = 0; i < 3000; i++) m = { ...m, ["p" + i]: i };
__L(2, "m1", Object.keys(m).length, m.x, m.p2999);
const ovr = { ...{ a: 1, b: 2 }, b: 3, ...{ c: 4 }, a: 5 };
__L(3, "ovr", JSON.stringify(ovr), Object.keys(ovr).join(","));
// chained spread through a Map and Set
const st = new Set([{ q: 1 }, { r: 2 }]);
__A("h02_spread_chain.js:st", function () { assert_eq(JSON.stringify([...st]), "[{\"q\":1},{\"r\":2}]", "st"); });

summary("bbreview");
