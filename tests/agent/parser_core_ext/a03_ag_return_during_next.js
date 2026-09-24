// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["r {\"value\":1,\"done\":false} {\"value\":2,\"done\":false} {\"value\":3,\"done\":false} {\"value\":\"stop\",\"done\":true}"];
__EXP[1] = ["after {\"done\":true}"];
__EXP[2] = ["r2 {\"value\":2,\"done\":false} {\"value\":\"early\",\"done\":true} g2fin"];
__EXP[3] = ["done2 {\"done\":true}"];
// A3: .return() issued while multiple .next() are still queued (the drain bug
// family) — pending nexts complete first, then the return settles, then the
// generator stays done; also return() racing a next that is suspended at await.
async function* agen() { yield 1; yield 2; yield 3; return "fin"; }
(async () => {
  const it = agen();
  const p1 = it.next(), p2 = it.next(), p3 = it.next();
  const pr = it.return("stop");
  __L(0, "r", JSON.stringify(await p1), JSON.stringify(await p2), JSON.stringify(await p3), JSON.stringify(await pr));
  __L(1, "after", JSON.stringify(await it.next()));
  // return while the generator is suspended INSIDE an await (try/finally must run)
  const events = [];
  async function* g2() {
    try { yield 1; await Promise.resolve(); yield 2; }
    finally { events.push("g2fin"); }
  }
  const it2 = g2();
  await it2.next();
  const q = it2.next();
  const r2 = it2.return("early");
  __L(2, "r2", JSON.stringify(await q), JSON.stringify(await r2), events.join(","));
  __L(3, "done2", JSON.stringify(await it2.next()));
})().catch(e => __L(4, "ERR", e.constructor.name, e.message));

__FINISH("parser_core_ext");
