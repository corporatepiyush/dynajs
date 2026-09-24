// minimal-failure search over (bits_a, bits_b) with dense random limbs
let s0 = 424242n;
function mkr(seed){ let s = seed; return function(n){ s = (s*6364136223846793005n+1442695040888963407n)&0xffffffffffffffffn; return s>>(64n-BigInt(n)); }; }
function trycase(bitsa, bitsb, nega, negb, rnd){
  let a=0n,b=0n;
  for(let i=0;i<(bitsa+31)/32;i++) a=(a<<32n)|(rnd(32)&0xffffffffn);
  for(let i=0;i<(bitsb+31)/32;i++) b=(b<<32n)|(rnd(32)&0xffffffffn);
  a = a >> BigInt(Math.max(0, 32*(bitsa+31)/32 - bitsa) | 0);
  b = b >> BigInt(Math.max(0, 32*(bitsb+31)/32 - bitsb) | 0);
  a &= (1n << BigInt(bitsa)) - 1n;
  b &= (1n << BigInt(bitsb)) - 1n;
  a |= (1n << BigInt(bitsa-1));
  b |= (1n << BigInt(bitsb-1));
  if (nega) a = -a;
  if (negb) b = -b;
  const p = a*b;
  return (p/a === b) && (p/b === a);
}
let fails = [];
for (const [ba, bb] of [[33,33],[40,40],[48,48],[64,64],[65,65],[70,33],[96,96],[128,64],[129,129],[200,200],[33,64],[70,70],[100,100]]) {
  let bad = -1;
  for (let t = 0; t < 40; t++) {
    const rnd = mkr(BigInt(1000+t));
    for (const [na, nb] of [[false,false],[true,false],[false,true],[true,true]]) {
      if (!trycase(ba, bb, na, nb, rnd)) { bad = t*10 + (na?2:0) + (nb?1:0); break; }
    }
    if (bad >= 0) break;
  }
  console.log(ba + "x" + bb + (bad >= 0 ? (" FAIL t/sign=" + bad) : " ok"));
}
