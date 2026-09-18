// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// A1: 3+ concurrent consumers on one async generator — FIFO queue order must
// hold across consumers with different pacing.
async function* agen(n) {
  for (let i = 0; i < n; i++) { await Promise.resolve(); yield i; }
}
(async () => {
  const it = agen(6);
  const order = [];
  async function consumer(tag, count) {
    for (let i = 0; i < count; i++) {
      const r = await it.next();
      order.push(tag + ":" + r.value);
    }
  }
  await Promise.all([consumer("A", 2), consumer("B", 2), consumer("C", 2)]);
  __A("a01_ag_multi_consumers.js:o1", function () { assert_eq(order.join(","), "A:0,B:1,C:2,A:3,B:4,C:5", "o1"); });
  const fin = await it.next();
  __A("a01_ag_multi_consumers.js:o2", function () { assert_eq(JSON.stringify(fin), "{\"done\":true}", "o2"); });
})().catch(e => __L(2, "ERR", e.constructor.name, e.message));

__FINISH("parser_core_ext");
