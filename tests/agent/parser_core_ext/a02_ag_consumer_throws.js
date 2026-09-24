// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["c1 {\"value\":\"s1\",\"done\":false}", "c2 {\"done\":true}", "c3 {\"done\":true}"];
// A2: a consumer that throws while others are queued — the generator queue
// must be untouched by consumer-side failures.
async function* agen() { yield 1; yield 2; yield 3; yield 4; }
(async () => {
  const it = agen();
  async function consumer(tag, fail) {
    const r = await it.next();
    if (fail) throw new Error("consumer-" + tag);
    return tag + r.value;
  }
  const ps = [consumer("A"), consumer("B", true), consumer("C"), consumer("D", true)];
  const out = [];
  for (const p of ps) {
    try { out.push(await p); } catch (e) { out.push("E:" + e.message); }
  }
  __A("a02_ag_consumer_throws.js:a", function () { assert_eq(out.join(","), "A1,E:consumer-B,C3,E:consumer-D", "a"); });
  const r = await it.next();
  __A("a02_ag_consumer_throws.js:b", function () { assert_eq(JSON.stringify(r), "{\"done\":true}", "b"); });
})().catch(e => __L(2, "ERR", e.constructor.name, e.message));
// consumer rejection racing with the generator's own completion
(async () => {
  async function* small() { yield "s1"; }
  const it2 = small();
  const q = [it2.next(), it2.next(), it2.next()];
  const failer = Promise.reject(new Error("pre-rejected"));
  for (const [tag, p] of [["1", q[0]], ["2", q[1]], ["3", q[2]]]) {
    try { __L(3, "c" + tag, JSON.stringify(await p)); } catch (e) { __L(4, "c" + tag, "E:" + e.message); }
  }
  try { await failer; } catch (e) { __A("a02_ag_consumer_throws.js:d", function () { assert_eq("E:" + e.message, "E:pre-rejected", "d"); }); }
})().catch(e => __L(6, "ERR2", e.constructor.name, e.message));

__FINISH("parser_core_ext");
