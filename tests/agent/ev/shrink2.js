let s = 999n; function rnd(n){ s = (s*6364136223846793005n+1442695040888963407n)&0xffffffffffffffffn; return s>>(64n-BigInt(n)); }
const bits=1+Number(rnd(12)%9000n); let a=0n,b=0n;
for(let i=0;i<bits/32;i++){a=(a<<32n)|(rnd(32)&0xffffffffn);b=(b<<32n)|(rnd(32)&0xffffffffn);}
a>>=1n;b>>=1n; if(rnd(1))a=-a; if(rnd(1))b=-b; if(a===0n)a=7n;
console.log("orig fails:", ((a*b)/a !== b));
// grid: truncate to k*32 bits (keep sign), find minimal failing k,l
const A = a < 0n ? -a : a, B = b < 0n ? -b : b, sa = a < 0n, sb = b < 0n;
let best = null;
for (let k = 4; k <= 100; k += 1) {
  for (let l = 4; l <= 100; l += 1) {
    const M1 = (1n << BigInt(32*k)) - 1n, M2 = (1n << BigInt(32*l)) - 1n;
    let x = (A & M1), y = (B & M2);
    x |= (1n << BigInt(32*k - 1)); y |= (1n << BigInt(32*l - 1));
    if (sa) x = -x;
    if (sb) y = -y;
    const p = x*y;
    if (p/x !== y || p/y !== x) { best = [k,l]; break; }
  }
  if (best) break;
}
console.log("minimal failing 32-bit-limb truncation:", best);
if (best) {
  const [k,l] = best;
  const M1 = (1n << BigInt(32*k)) - 1n, M2 = (1n << BigInt(32*l)) - 1n;
  let x = (A & M1) | (1n << BigInt(32*k-1)), y = (B & M2) | (1n << BigInt(32*l-1));
  if (sa) x = -x; if (sb) y = -y;
  console.log("x:", x.toString());
  console.log("y:", y.toString());
}
