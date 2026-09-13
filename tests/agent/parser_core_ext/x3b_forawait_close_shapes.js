// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[6] = ["T7 fin"];
__EXP[7] = ["T7 {\"value\":\"y1\",\"done\":false} {\"value\":\"y2\",\"done\":false}"];
__EXP[8] = ["T7 {\"value\":\"r\",\"done\":true}"];
// x3 differential: for-await abrupt completions must await the close.
const out = console.log;
function trySync(s) { try { return s(); } catch (e) { return "E:" + e.message; } }

// T1: throw from body — close awaited before the error surfaces
async function t1() {
  const log = [];
  async function* g() { try { yield 1; yield 2; } finally { log.push("fin1"); } }
  try { for await (const v of g()) { log.push("v" + v); if (v === 1) throw new Error("boom"); } }
  catch (e) { log.push("E:" + e.message); }
  return log.join(",");
}
t1().then(r => __A("x3b_forawait_close_shapes.js:T1", function () { assert_eq(r, "v1,fin1,E:boom", "T1"); }));

// T2: return from body — close awaited before the promise resolves
async function t2() {
  const log = [];
  async function* g() { try { yield 1; } finally { log.push("fin2"); } }
  for await (const v of g()) { log.push("v" + v); return log.join(","); }
}
t2().then(r => __A("x3b_forawait_close_shapes.js:T2", function () { assert_eq(r, "v1", "T2"); }));

// T3: break with REJECTING close — rejection replaces the break
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
t3().then(r => __A("x3b_forawait_close_shapes.js:T3", function () { assert_eq(r, "v1,E:close-fail", "T3"); }));

// T4: close absent — break proceeds without close
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
t4().then(r => __A("x3b_forawait_close_shapes.js:T4", function () { assert_eq(r, "v1,broke", "T4"); }));

// T5: close returns non-object — TypeError replaces break
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
t5().then(r => __A("x3b_forawait_close_shapes.js:T5", function () { assert_eq(r, "v,E:TypeError", "T5"); }));

// T6: close-during-pending-next: manual nexts racing for-await break
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
t6().then(r => __A("x3b_forawait_close_shapes.js:T6", function () { assert_eq(r, "v3,fin6,broke6,{\"value\":1,\"done\":false}{\"value\":2,\"done\":false}", "T6"); }));

// T7: for-await inside async generator, yield in body, then gen.return()
async function* t7() {
  async function* g() { try { yield 1; yield 2; } finally { __L(6, "T7 fin"); } }
  for await (const v of g()) { yield "y" + v; }
  return "done7";
}
(async () => {
  const gi = t7();
  __L(7, "T7", JSON.stringify(await gi.next()), JSON.stringify(await gi.next()));
  __L(8, "T7", JSON.stringify(await gi.return("r")));
})();

// T8: continue inside for-await: no close on continue, close at break later
async function t8() {
  const log = [];
  async function* g() { try { yield 1; yield 2; yield 3; } finally { log.push("fin8"); } }
  let n = 0;
  for await (const v of g()) { n++; if (v === 1) continue; log.push("v" + v); if (n >= 3) break; }
  return log.join(",");
}
t8().then(r => __A("x3b_forawait_close_shapes.js:T8", function () { assert_eq(r, "v2,v3,fin8", "T8"); }));

// T9: labeled break out of nested for-await
async function t9() {
  const log = [];
  async function* mk(n) { try { yield 1; yield 2; } finally { log.push("fin" + n); } }
  outer: for await (const v of mk(1)) {
    log.push("v" + v);
    for await (const w of mk(2)) { log.push("w" + w); break outer; }
  }
  return log.join(",");
}
t9().then(r => __A("x3b_forawait_close_shapes.js:T9", function () { assert_eq(r, "v1,w1,fin2,fin1", "T9"); }));

// T10: sync iterator via for-await (async-from-sync close)
async function t10() {
  const log = [];
  function* g() { try { yield 1; yield 2; } finally { log.push("fin10"); } }
  for await (const v of g()) { log.push("v" + v); break; }
  return log.join(",");
}
t10().then(r => __A("x3b_forawait_close_shapes.js:T10", function () { assert_eq(r, "v1,fin10", "T10"); }));

__FINISH("parser_core_ext");
