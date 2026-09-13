// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1 {\"value\":1,\"done\":false}", "r {\"value\":\"r\",\"done\":true}"];
__EXP[1] = ["2 E:gen-boom", "t E:ext"];
__EXP[2] = ["post {\"done\":true}"];
__EXP[4] = ["q {\"value\":2,\"done\":false}"];
__EXP[6] = ["t E:injected caught:injected,g2fin"];
// X4: DIVERGENCE DOCUMENTATION - queued .throw() behind a completed async
// generator NEVER SETTLES (await hangs, output truncated) when the generator
// completed via its own throw during a next() resume. node settles E:ext.
// Pre-existing in baseline; the drain fix did not cover this order.
// A5: .throw() from outside queued BEHIND pending nexts and a queued .return()
// — settle order and final state must match the spec queue semantics.
async function* agen() { yield 1; throw new Error("gen-boom"); }
(async () => {
  const it = agen();
  const p1 = it.next(), p2 = it.next(), pr = it.return("r"), pt = it.throw(new Error("ext"));
  for (const [tag, p] of [["1", p1], ["2", p2], ["r", pr], ["t", pt]]) {
    try { __L(0, tag, JSON.stringify(await p)); } catch (e) { __L(1, tag, "E:" + e.message); }
  }
  __L(2, "post", JSON.stringify(await it.next()));
})().catch(e => __L(3, "ERR", e.constructor.name, e.message));
// throw() while the generator is suspended at an await inside try (finally runs)
(async () => {
  const events = [];
  async function* g2() {
    try { yield 1; await Promise.resolve(); yield 2; }
    catch (e) { events.push("caught:" + e.message); throw e; }
    finally { events.push("g2fin"); }
  }
  const it = g2();
  await it.next();
  const q = it.next();
  const t = it.throw(new Error("injected"));
  __L(4, "q", JSON.stringify(await q));
  try { __L(5, "t", JSON.stringify(await t)); } catch (e) { __L(6, "t", "E:" + e.message, events.join(",")); }
})().catch(e => __L(7, "ERR2", e.constructor.name, e.message));

__FINISH("parser_core_ext");
