// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = [["indirect-eval: rejected TypeError", "indirect-eval: rejected Error"], ["newFunction: rejected ReferenceError", "newFunction: rejected Error"]];
__EXP[3] = ["eval-static-syntax: sync number"];
__REQ = {"dynajs": {"2": 2, "3": 1}, "node": {"2": 2, "3": 1}};
// modules_ext g04 (dynajs-only): import of dyna: modules from EVAL'd code: indirect
// eval and new Function. Current-vs-current causality (node lacks dyna:); output
// must be byte-stable across repeated runs of the same binary.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
function rec(tag, f) { try { const r = f(); if (r && typeof r.then === 'function') { r.then(x => __L(1, tag + ': ' + (x === undefined ? 'undefined' : typeof x)), e => __L(2, tag + ': rejected ' + ((e && e.name) || 'thrown'))).then(() => done()); } else __L(3, tag + ': sync ' + typeof r); } catch (e) { __L(4, tag + ': threw ' + ((e && e.name) || 'thrown')); } }
let dones = 0;
function done() { if (++dones === 3) __L(5, 'DONE'); }
rec('indirect-eval', () => (0, eval)('import("dyna:bytes")'));
rec('newFunction', () => new Function('return import("dyna:semver")')());
rec('eval-static-syntax', () => eval('1 + 1'));

__FINISH("modules_ext");
