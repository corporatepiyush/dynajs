__EXP = {};
__EXP[1] = ["keys: len=1000 ownKeys=1 gopd=1000 get=0"];
__EXP[2] = ["values: len=1000 ownKeys=1 gopd=1000 get=1000"];
__EXP[3] = ["entries: len=1000 ownKeys=1 gopd=1000 get=1000"];
__EXP[4] = ["gopn: len=1000 ownKeys=1 gopd=0 get=0"];
__EXP[5] = ["gops: ownKeys=1 gopd=0 get=0"];
__EXP[6] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const target = {};
for (let i = 0; i < 1000; i++) target['k' + i] = i;
let ownKeysCalls = 0, gopdCalls = 0, getCalls = 0;
const p = new Proxy(target, {
  ownKeys(t) { ownKeysCalls++; return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor(t, k) { gopdCalls++; return Reflect.getOwnPropertyDescriptor(t, k); },
  get(t, k) { getCalls++; return t[k]; },
});
ownKeysCalls = gopdCalls = getCalls = 0;
const k = Object.keys(p);
__L(1, 'keys: len=' + k.length + ' ownKeys=' + ownKeysCalls + ' gopd=' + gopdCalls + ' get=' + getCalls);
ownKeysCalls = gopdCalls = getCalls = 0;
const v = Object.values(p);
__L(2, 'values: len=' + v.length + ' ownKeys=' + ownKeysCalls + ' gopd=' + gopdCalls + ' get=' + getCalls);
ownKeysCalls = gopdCalls = getCalls = 0;
const e = Object.entries(p);
__L(3, 'entries: len=' + e.length + ' ownKeys=' + ownKeysCalls + ' gopd=' + gopdCalls + ' get=' + getCalls);
ownKeysCalls = gopdCalls = getCalls = 0;
const n = Object.getOwnPropertyNames(p);
__L(4, 'gopn: len=' + n.length + ' ownKeys=' + ownKeysCalls + ' gopd=' + gopdCalls + ' get=' + getCalls);
ownKeysCalls = gopdCalls = getCalls = 0;
Object.getOwnPropertySymbols(p);
__L(5, 'gops: ownKeys=' + ownKeysCalls + ' gopd=' + gopdCalls + ' get=' + getCalls);
__L(6, 'DONE');

summary("modules_ext");
