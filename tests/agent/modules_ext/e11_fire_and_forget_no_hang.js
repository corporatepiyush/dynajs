// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["a:v1dfalse|b:vundefineddtrue|c:vundefineddtrue|d:vundefineddtrue|e:vundefineddtrue"];
__EXP[2] = ["DONE"];
// modules_ext e11: THE hang regression: fire-and-forget next() requests on a
// generator that completes (or was already completed) must ALL resolve; the job
// queue must drain so the process exits after DONE. No awaited chaining.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  yield 1;
}
const gen = g();
gen.next().then(r => log.push('a:v' + r.value + 'd' + r.done)).catch(e => log.push('a:E' + (e && e.name)));
// queue 3 more BEFORE the first resolves; generator has only one yield
gen.next().then(r => log.push('b:v' + r.value + 'd' + r.done)).catch(e => log.push('b:E' + (e && e.name)));
gen.next().then(r => log.push('c:v' + r.value + 'd' + r.done)).catch(e => log.push('c:E' + (e && e.name)));
gen.next().then(r => log.push('d:v' + r.value + 'd' + r.done)).catch(e => log.push('d:E' + (e && e.name)));
// and one AFTER everything settled
Promise.resolve().then(() => Promise.resolve()).then(() => {
  gen.next().then(r => log.push('e:v' + r.value + 'd' + r.done)).catch(e => log.push('e:E' + (e && e.name)));
  Promise.resolve().then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });
});

__FINISH("modules_ext");
