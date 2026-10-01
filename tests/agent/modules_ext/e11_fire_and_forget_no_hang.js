__EXP = {};
__EXP[1] = ["a:v1dfalse|b:vundefineddtrue|c:vundefineddtrue|d:vundefineddtrue|e:vundefineddtrue"];
__EXP[2] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  yield 1;
}
const gen = g();
gen.next().then(r => log.push('a:v' + r.value + 'd' + r.done)).catch(e => log.push('a:E' + (e && e.name)));
gen.next().then(r => log.push('b:v' + r.value + 'd' + r.done)).catch(e => log.push('b:E' + (e && e.name)));
gen.next().then(r => log.push('c:v' + r.value + 'd' + r.done)).catch(e => log.push('c:E' + (e && e.name)));
gen.next().then(r => log.push('d:v' + r.value + 'd' + r.done)).catch(e => log.push('d:E' + (e && e.name)));
Promise.resolve().then(() => Promise.resolve()).then(() => {
  gen.next().then(r => log.push('e:v' + r.value + 'd' + r.done)).catch(e => log.push('e:E' + (e && e.name)));
  Promise.resolve().then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });
});

__FINISH("modules_ext");
