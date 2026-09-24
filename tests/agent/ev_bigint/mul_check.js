// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = [["~alen_bits: 3007", "~alen_bits: 3007"]];
let s = 999n; function rnd(n){ s = (s*6364136223846793005n+1442695040888963407n)&0xffffffffffffffffn; return s>>(64n-BigInt(n)); }
const bits=1+Number(rnd(12)%9000n); let a=0n,b=0n;
for(let i=0;i<bits/32;i++){a=(a<<32n)|(rnd(32)&0xffffffffn);b=(b<<32n)|(rnd(32)&0xffffffffn);}
a>>=1n;b>>=1n; if(rnd(1))a=-a; if(rnd(1))b=-b; if(a===0n)a=7n;
const p=a*b;
__A("mul_check.js:ok:", function () { assert_eq(p/a===b && p/b===a, true, "ok:"); });
// print compact witnesses for the C-level repro
__A("mul_check.js:a_hex_head:", function () { assert_eq(a.toString(16).slice(0,32), "-338ac722888b747ca2ecfe250b10509", "a_hex_head:"); });
__L(2, "alen_bits:", a.toString(2).length, "blen_bits:", b.toString(2).length);
globalThis.__w = [a, b];

summary("ev_bigint");
