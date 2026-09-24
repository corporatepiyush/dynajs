// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["k1 1 1 c {\"c\":1}"];
__EXP[1] = ["k2 2 c"];
__EXP[2] = ["k3 97 122 26 abcdefghijklmnopqrstuvwxyz"];
__EXP[3] = ["k4 a 1", "k4 b 2", "k4 c 3", "k4 d 4"];
__EXP[4] = ["k5 11 x"];
__EXP[5] = ["k6 h1,e1,l2,o1 2"];
__EXP[6] = ["k7 {\"b\":1,\"a\":3} b,a"];
__EXP[7] = ["k8 99 p"];
// F: cached 1-char strings as property keys (atom donation interplay)
const [c] = "c".split("");
const o = {};
o[c] = 1;
__L(0, "k1", o.c, o["c"], Object.keys(o).join(","), JSON.stringify(o));
delete o[c];
o.c = 2;
__L(1, "k2", o[c], Object.keys(o).join(","));
const chars = "abcdefghijklmnopqrstuvwxyz".split("");
const obj = {};
for (const ch of chars) obj[ch] = ch.charCodeAt(0);
__L(2, "k3", obj.a, obj.z, Object.keys(obj).length, Object.keys(obj).join(""));
const lit = { a: 1, b: 2, c: 3, d: 4 };
for (const ch of "abcd".split("")) __L(3, "k4", ch, lit[ch]);
const m = {};
m.x = 10;
delete m["x"];
m["x"] = 11;
__L(4, "k5", m.x, Object.keys(m).join(","));
const mp = new Map();
for (const ch of "hello".split("")) mp.set(ch, (mp.get(ch) || 0) + 1);
__L(5, "k6", [...mp].map(([k, v]) => k + v).join(","), mp.get("l"));
// 1-char key colliding with cached char used as object literal key order
const mixed = {};
mixed.b = 1;
mixed["a".split("")[0]] = 2;
mixed.a = 3;
__L(6, "k7", JSON.stringify(mixed), Object.keys(mixed).join(","));
// delete/re-add loop 100x with cached keys
const tgt = { p: 0 };
for (let i = 0; i < 100; i++) {
  const [k] = "p".split("");
  delete tgt[k];
  tgt[k] = i;
}
__L(7, "k8", tgt.p, Object.keys(tgt).join(","));

summary("bbreview");
