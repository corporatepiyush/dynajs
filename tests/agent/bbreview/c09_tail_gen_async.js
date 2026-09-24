// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[4] = [["af-deep-overflow InternalError", "af-deep-overflow RangeError"]];
__EXP[5] = ["ag {\"value\":0,\"done\":false}", "ag {\"value\":1,\"done\":false}", "ag {\"value\":2,\"done\":false}", "ag {\"value\":3,\"done\":false}", "ag {\"value\":\"end\",\"done\":true}"];
__EXP[6] = ["end"];
__EXP[9] = [["ap-deep-overflow InternalError", "ap-deep-overflow RangeError"]];
__REQ = {"dynajs": {"4": 1, "5": 5, "6": 1, "9": 1}, "node": {"4": 1, "5": 5, "6": 1, "9": 1}};
// C: generators/async functions containing `return g(x)` — NOT tail, must still work
function* gen(n) {
  for (let i = 0; i < n; i++) yield i;
  return "gen-done-" + n;
}
function* g2(n) { const r = yield* gen(n); return r; }
__A("c09_tail_gen_async.js:gen1", function () { assert_eq([...g2(3)].join(","), "0,1,2", "gen1"); });
__A("c09_tail_gen_async.js:gen2", function () { assert_eq([...gen(0)].join(","), "", "gen2"); });
async function af(n) { if (n === 0) return "async-done"; return af(n - 1); }
af(5).then(v => __A("c09_tail_gen_async.js:af5", function () { assert_eq(v, "async-done", "af5"); }));
af(10000).then(v => __L(3, "af-deep", v), e => __L(4, "af-deep-overflow", e.constructor.name));
async function* agen(n) { for (let i = 0; i < n; i++) yield i; return "end"; }
(async () => {
  const it = agen(4);
  const ps = [it.next(), it.next(), it.next(), it.next(), it.next()];
  for (const p of ps) __L(5, "ag", JSON.stringify(await p));
  __L(6, "end");
})().catch(e => __L(7, "AG-ERR", e.constructor.name, e.message));
// async fn tail-returning a promise
async function ap(n) { if (n === 0) return "ap-done"; return ap(n - 1); }
ap(200000).then(v => __L(8, "ap-deep", v), e => __L(9, "ap-deep-overflow", e.constructor.name));

__FINISH("bbreview");
