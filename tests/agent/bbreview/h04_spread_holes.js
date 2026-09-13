// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["h1 1000 true true true 0 999"];
__EXP[1] = ["h2 [1,null,3] true 3"];
__EXP[2] = ["h3 501 true true 501"];
__EXP[3] = ["h4 4 3 1,2,3,4"];
__EXP[4] = ["h5 1,,3 0,,,,"];
__EXP[5] = ["h6 1,2,3,1,2,3 99,2,3"];
__EXP[8] = ["h9 {\"0\":1,\"1\":2,\"2\":3,\"500\":501} 4"];
__EXP[9] = [["~h10 1001 tail", "~h10 1001 tail"]];
__EXP[10] = ["h11 10 0,,,,,,,,,"];
// H: spreads with holes + push-after-spread length sync
const holey = new Array(1000);
holey[0] = 0; holey[999] = 999;
const out = [...holey];
__L(0, "h1", out.length, 0 in out, 1 in out, 999 in out, out[0], out[999]);
const mid = [1, , 3];
const o2 = [...mid];
__L(1, "h2", JSON.stringify(o2), 1 in o2, o2.length);
const sparse = [1, 2, 3];
sparse[500] = 501;
const o3 = [...sparse];
__L(2, "h3", o3.length, 499 in o3, 500 in o3, o3[500]);
const base = [1, 2, 3];
const copy = [...base];
copy.push(4);
__L(3, "h4", copy.length, base.length, copy.join(","));
function id() { return Array.from(arguments, (v, i) => (i in arguments ? v : "H")).join(","); }
__L(4, "h5", id(...mid), id(...holey.slice(0, 5)));
const self = [1, 2, 3];
const s2 = [...self, ...self];
self[0] = 99;
__L(5, "h6", s2.join(","), self.join(","));
const u = new Uint8Array([5, 6, 7]);
__A("h04_spread_holes.js:h7", function () { assert_eq(JSON.stringify([...u]), "[5,6,7]", "h7"); });
const st = new Set([1, 2, 3]);
__A("h04_spread_holes.js:h8", function () { assert_eq(JSON.stringify([...st]), "[1,2,3]", "h8"); });
// spread-into-object of array (index keys)
__L(8, "h9", JSON.stringify({ ...sparse }), Object.keys({ ...sparse }).length);
// 1000-hole spread then .push then .pop
const p = [...holey];
p.push("tail");
__L(9, "h10", p.length, p[p.length - 1], p.pop(), p.length);
// spread where the result becomes holey then is sorted (no fast-array length desync)
const sh = [...holey.slice(0, 10)];
sh.sort();
__L(10, "h11", sh.length, sh.join(","));

summary("bbreview");
