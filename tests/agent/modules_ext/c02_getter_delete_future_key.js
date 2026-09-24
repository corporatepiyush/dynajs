// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["del-future-values=[\"A\",1,3,5,7]"];
__EXP[2] = ["del-next-values=[1,3]"];
__EXP[3] = ["keys-after=[\"a\",\"c\"]"];
__EXP[4] = ["del-future-entries=[[\"a\",1],[\"b\",2],[\"d\",4]]"];
__EXP[5] = ["del-all-values=[\"Z\"] len=1"];
__EXP[6] = ["DONE"];
// modules_ext c02: getter DELETES future keys during values/entries. The per-key
// re-check must drop them (test262 Object/values/getter-removing-future-key class),
// for both bulk (fast-array) and small paths, and for entries too.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const o = {};
for (let i = 0; i < 8; i++) o['k' + i] = i;
Object.defineProperty(o, 'k0', { get() { delete o.k2; delete o.k4; delete o.k6; return 'A'; }, enumerable: true, configurable: true });
__L(1, 'del-future-values=' + JSON.stringify(Object.values(o)));
const o2 = { a: 1, b: 2, c: 3 };
Object.defineProperty(o2, 'a', { get() { delete o2.b; return 1; }, enumerable: true, configurable: true });
__L(2, 'del-next-values=' + JSON.stringify(Object.values(o2)));
__L(3, 'keys-after=' + JSON.stringify(Object.keys(o2)));
const o3 = { a: 1, b: 2, c: 3, d: 4 };
Object.defineProperty(o3, 'a', { get() { delete o3.c; return 1; }, enumerable: true, configurable: true });
__L(4, 'del-future-entries=' + JSON.stringify(Object.entries(o3)));
// big object (bulk path) deleting ALL future keys -> result shrinks below presize
const o4 = {};
for (let i = 0; i < 100; i++) o4['f' + i] = i;
Object.defineProperty(o4, 'f0', { get() { for (let i = 1; i < 100; i++) delete o4['f' + i]; return 'Z'; }, enumerable: true, configurable: true });
const v4 = Object.values(o4);
__L(5, 'del-all-values=' + JSON.stringify(v4) + ' len=' + v4.length);
__L(6, 'DONE');

summary("modules_ext");
