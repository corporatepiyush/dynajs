// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["afin", "afin"];
__EXP[1] = ["v 1", "v 2"];
__EXP[2] = ["broke"];
__EXP[4] = ["q {\"value\":1,\"done\":false} {\"value\":2,\"done\":false}"];
__EXP[5] = ["post {\"done\":true}"];
// X3: DIVERGENCE DOCUMENTATION (expected FAIL vs node; baseline-equal):
// for-await break close ordering — node awaits the iterator close before
// continuing after the loop; dynajs and the pristine baseline continue first
// and run the close later. Also covers implicit .return() racing manual
// .next() calls queued on the same generator.
// also queued on the same generator; drain state must stay coherent.
async function* agen() {
  try { yield 1; yield 2; yield 3; }
  finally { __L(0, "afin"); }
}
(async () => {
  for await (const v of agen()) { __L(1, "v", v); if (v === 2) break; }
  __L(2, "broke");
  // manual nexts interleaved with a for-await loop over the SAME generator
  const g2 = agen();
  const i2 = g2.next(), i3 = g2.next();
  for await (const v of { [Symbol.asyncIterator]() { return g2; } }) {
    __A("x3_ag_forawait_close_order.js:w", function () { assert_eq(v, 3, "w"); });
    break;
  }
  __L(4, "q", JSON.stringify(await i2), JSON.stringify(await i3));
  // after the break, generator must be finished
  __L(5, "post", JSON.stringify(await g2.next()));
})().catch(e => __L(6, "ERR", e.constructor.name, e.message));

__FINISH("parser_core_ext");
