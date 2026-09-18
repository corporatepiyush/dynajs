// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["j1 40001 [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\""];
__EXP[1] = ["j2 10000 true 1"];
__EXP[2] = ["j3 \"\\\"\" [\"\\\"\",\"\\\"\"]", "j3 \"\\\\\" [\"\\\\\",\"\\\\\"]", "j3 \"\\n\" [\"\\n\",\"\\n\"]", "j3 \"\\u0000\" [\"\\u0000\",\"\\u0000\"]", "j3 \"\u2028\" [\"\u2028\",\"\u2028\"]", "j3 \"\u2029\" [\"\u2029\",\"\u2029\"]", "j3 \"\u007f\" [\"\u007f\",\"\u007f\"]", "j3 \"\ufeff\" [\"\ufeff\",\"\ufeff\"]"];
__EXP[3] = ["j4 40001 [\"x\",\"x\",\""];
__EXP[4] = ["j5 {\"0\":26,\"1\":27,\"2\":28,\"3\":29,\"4\":30,\"5\":31,\"6\":32,\"7\":33,\"8\":34,\"9\":35,\"a\":0,\"b\":1,\"c\":2,\"d\":3,\"e\":4,\"f\":5,\"g\":6,\"h\":7,\"i\":8,\"j\":9,\"k\":10,\"l\":11,\"m\":12,\"n\":13,\"o\":14,\"p\":15,\"q\":16,\"r\":17,\"s\":18,\"t\":19,\"u\":20,\"v\":21,\"w\":22,\"x\":23,\"y\":24,\"z\":25} 36"];
__EXP[5] = ["j6 [\"\u00e9\",\"\u4e2d\",\"\ud83d\ude00\"] 14"];
// F: JSON.stringify of 10k cached 1-char strings
const chars = "abcdefghijklmnopqrstuvwxyz0123456789".split("");
const arr = [];
for (let i = 0; i < 10000; i++) arr.push(chars[i % chars.length]);
const json = JSON.stringify(arr);
__L(0, "j1", json.length, json.slice(0, 40));
const back = JSON.parse(json);
__L(1, "j2", back.length, back[0] === "a", back[9999]);
for (const s of ['"', "\\", "\n", "\u0000", "\u2028", "\u2029", "\u007f", "\ufeff"]) {
  __L(2, "j3", JSON.stringify(s), JSON.stringify([s, s]));
}
const same = new Array(10000).fill("x");
const js2 = JSON.stringify(same);
__L(3, "j4", js2.length, js2.slice(0, 10));
// object with cached-char keys
const oc = {};
for (let i = 0; i < 36; i++) oc[chars[i]] = i;
const j5 = JSON.stringify(oc);
__L(4, "j5", j5, Object.keys(JSON.parse(j5)).length);
// stringify wide cached strings
const wide = ["\u00e9", "\u4e2d", "\u{1F600}"];
__L(5, "j6", JSON.stringify(wide), JSON.stringify(wide).length);

summary("bbreview");
