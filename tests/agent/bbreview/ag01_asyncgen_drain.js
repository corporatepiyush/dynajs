// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["k1 0,", "k3 0,1,2,", "k4 0,1,2,3,", "k8 0,1,2,3,4,5,6,7,", "k64 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,"];
__EXP[1] = ["mix {\"value\":0,\"done\":false} {\"done\":true} {\"value\":\"early\",\"done\":true}"];
__EXP[2] = ["th {\"value\":1,\"done\":false}", "th {\"done\":true}"];
__EXP[4] = ["end"];
// commit 1 area: async-generator queue drain (>=4 concurrent consumers)
async function* agen(n) { for (let i = 0; i < n; i++) yield i; }
(async () => {
  for (const k of [1, 3, 4, 8, 64]) {
    const it = agen(k);
    const ps = [];
    for (let i = 0; i < k + 1; i++) ps.push(it.next());
    const vals = [];
    for (const p of ps) vals.push((await p).value);
    __L(0, "k" + k, vals.join(","));
  }
  const it2 = agen(1);
  const p1 = it2.next(), p2 = it2.next(), p3 = it2.return("early");
  __L(1, "mix", JSON.stringify(await p1), JSON.stringify(await p2), JSON.stringify(await p3));
  // throw queued behind a next
  async function* boomGen() { yield 1; throw new Error("gen-throw"); yield 3; }
  const it3 = boomGen();
  const q = [it3.next(), it3.next(), it3.next()];
  for (const p of q) {
    try { __L(2, "th", JSON.stringify(await p)); } catch (e) { __A("ag01_asyncgen_drain.js:th", function () { assert_eq("E:" + e.message, "E:gen-throw", "th"); }); }
  }
  __L(4, "end");
})().catch(e => __L(5, "ERR", e.constructor.name, e.message));

__FINISH("bbreview");
