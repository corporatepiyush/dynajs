const out = console.log;
function trySync(s) { try { return s(); } catch (e) { return "E:" + e.message; } }

async function t1() {
  const log = [];
  async function* g() { try { yield 1; yield 2; } finally { log.push("fin1"); } }
  try { for await (const v of g()) { log.push("v" + v); if (v === 1) throw new Error("boom"); } }
  catch (e) { log.push("E:" + e.message); }
  return log.join(",");
}
t1().then(r => out("T1", r));

async function t2() {
  const log = [];
  async function* g() { try { yield 1; } finally { log.push("fin2"); } }
  for await (const v of g()) { log.push("v" + v); return log.join(","); }
}
t2().then(r => out("T2", r));

async function t3() {
  const log = [];
  const g = {
    [Symbol.asyncIterator]() {
      let i = 0;
      return {
        async next() { return { value: ++i, done: false }; },
        async return() { throw new Error("close-fail"); },
      };
    }
  };
  try { for await (const v of g) { log.push("v" + v); break; } log.push("broke"); }
  catch (e) { log.push("E:" + e.message); }
  return log.join(",");
}
t3().then(r => out("T3", r));

async function t4() {
  const log = [];
  const g = {
    [Symbol.asyncIterator]() {
      let i = 0;
      return { async next() { return { value: ++i, done: i > 5 }; } };
    }
  };
  for await (const v of g) { log.push("v" + v); break; }
  log.push("broke");
  return log.join(",");
}
t4().then(r => out("T4", r));

async function t5() {
  const log = [];
  const g = {
    [Symbol.asyncIterator]() {
      return { async next() { return { value: 1, done: false }; },
               async return() { return 42; } };
    }
  };
  try { for await (const v of g) { log.push("v"); break; } log.push("broke"); }
  catch (e) { log.push("E:" + e.constructor.name); }
  return log.join(",");
}
t5().then(r => out("T5", r));

async function t6() {
  const log = [];
  async function* g() { try { yield 1; yield 2; yield 3; } finally { log.push("fin6"); } }
  const it = g();
  const p1 = it.next(), p2 = it.next();
  for await (const v of { [Symbol.asyncIterator]() { return it; } }) { log.push("v" + v); break; }
  log.push("broke6");
  log.push(JSON.stringify(await p1) + JSON.stringify(await p2));
  return log.join(",");
}
t6().then(r => out("T6", r));

async function* t7() {
  async function* g() { try { yield 1; yield 2; } finally { out("T7 fin"); } }
  for await (const v of g()) { yield "y" + v; }
  return "done7";
}
(async () => {
  const gi = t7();
  out("T7", JSON.stringify(await gi.next()), JSON.stringify(await gi.next()));
  out("T7", JSON.stringify(await gi.return("r")));
})();

async function t8() {
  const log = [];
  async function* g() { try { yield 1; yield 2; yield 3; } finally { log.push("fin8"); } }
  let n = 0;
  for await (const v of g()) { n++; if (v === 1) continue; log.push("v" + v); if (n >= 3) break; }
  return log.join(",");
}
t8().then(r => out("T8", r));

async function t9() {
  const log = [];
  async function* mk(n) { try { yield 1; yield 2; } finally { log.push("fin" + n); } }
  outer: for await (const v of mk(1)) {
    log.push("v" + v);
    for await (const w of mk(2)) { log.push("w" + w); break outer; }
  }
  return log.join(",");
}
t9().then(r => out("T9", r));

async function t10() {
  const log = [];
  function* g() { try { yield 1; yield 2; } finally { log.push("fin10"); } }
  for await (const v of g()) { log.push("v" + v); break; }
  return log.join(",");
}
t10().then(r => out("T10", r));
