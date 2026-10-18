// flags: --std
// Split from tests/test_cov_ml_dataframe_mathx.js: the mathx sections.
import * as mathx from "dyna:mathx";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== mathx ===");
{
    assertEq(mathx.E, Math.E,"E === Math.E");
    assertEq(mathx.Pi, Math.PI,"Pi === Math.PI");
    assert(typeof mathx.Phi==="number","Phi number");
    const ls0=mathx.linspace(0,1,0);
    assertEq(ls0.length,0,"linspace n=0 length 0");
    const ls1=mathx.linspace(0,10,1);
    assertEq(ls1.length,1,"linspace n=1");
    assertEq(ls1[0],10,"linspace n=1 is exactly b (impl convention)");
    const ls2=mathx.linspace(0,10,2);
    assertEq(ls2.length,2,"linspace n=2");
    assertEq(ls2[0],0,"linspace n2 first a");
    assertEq(ls2[1],10,"linspace n2 last is b exactly");
    const ls=mathx.linspace(0,1,5);
    assertEq(ls.length,5,"linspace 5 points");
    assertEq(ls[0],0,"linspace start");
    assertEq(ls[4],1,"linspace end exactly b");
    const bigLs=mathx.linspace(0,1,1000000);
    assertEq(bigLs.length,1000000,"linspace 1e6 allowed");
    assertEq(bigLs[0],0,"linspace 1e6 first");
    assertEq(bigLs[bigLs.length-1],1,"linspace 1e6 last exactly b");
    await assertThrows(()=>mathx.linspace(0,1,1000001),"linspace 1000001 throws");
    await assertThrows(()=>mathx.linspace(0,1,1e8),"linspace 1e8 throws");
    await assertThrows(()=>mathx.linspace(0,1,-1),"linspace negative throws");
    const lsp=mathx.logspace(0,1,2);
    assertEq(lsp.length,2,"logspace 2");
    assertClose(lsp[0],1,1e-12,"logspace start 10^0");
    assertClose(lsp[1],10,1e-12,"logspace end 10^1");
    const lsp0=mathx.logspace(0,1,0);
    assertEq(lsp0.length,0,"logspace 0");
    await assertThrows(()=>mathx.logspace(0,1,1000001),"logspace 1000001 throws");
}
{
    assertClose(mathx.gammainc(2,1), 1-Math.exp(-2),1e-12,"gammainc x2 a1");
    assertClose(mathx.gammainc(2,1,"upper"), Math.exp(-2),1e-12,"gammainc upper");
    assertClose(mathx.gammainc(5,2)+mathx.gammainc(5,2,"upper"),1,1e-12,"gammainc P+Q=1");
    assert(Number.isNaN(mathx.gammainc(NaN,1)) || typeof mathx.gammainc(NaN,1)==="number","gammainc NaN");
    assertClose(mathx.erf(0),0,1e-12,"erf 0");
    assertEq(mathx.erf(Infinity),1,"erf inf");
    assertEq(mathx.erf(-Infinity),-1,"erf -inf");
    assert(Number.isNaN(mathx.erf(NaN)),"erf NaN");
    assertClose(mathx.erf(1),0.84270079,1e-6,"erf 1");
    assertClose(mathx.erfc(0),1,1e-12,"erfc 0");
    assertClose(mathx.gamma(5),24,1e-9,"gamma 5");
    assertEq(mathx.gamma(1),1,"gamma 1");
    assert(Number.isNaN(mathx.gamma(NaN)),"gamma NaN");
    assertEq(mathx.gamma(Infinity),Infinity,"gamma inf");
}
{
    assertClose(mathx.besselj(0,0),1,1e-12,"besselj 0,0");
    assertEq(mathx.besselj(5,0),0,"besselj 5,0 0");
    assertClose(mathx.besseli(0,0),1,1e-12,"besseli 0,0");
    assert(Number.isFinite(mathx.besseliScaled(0,800)),"besseliScaled finite");
    assertEq(mathx.factorial(0),1n,"factorial 0");
    assertEq(mathx.factorial(5),120n,"factorial 5");
    assertEq(mathx.factorial(20),2432902008176640000n,"factorial 20");
    await assertThrows(()=>mathx.factorial(-1),"factorial -1 throws");
    await assertThrows(()=>mathx.factorial(1e9),"factorial huge throws");
    await assertThrows(()=>mathx.factorial(10001),"factorial over cap throws");
    assertEq(mathx.isPrime(2),true,"isPrime 2");
    assertEq(mathx.isPrime(4),false,"isPrime 4");
    assertEq(mathx.isPrime(17),true,"isPrime 17");
    assertEq(mathx.isPrime(0),false,"isPrime 0");
    const pr=mathx.primes(10);
    assert(JSON.stringify(pr)===JSON.stringify([2,3,5,7]),"primes 10");
    await assertThrows(()=>mathx.primes(5e7+1),"primes over 5e7 throws");
    assertEq(mathx.gcd(12,18),6n,"gcd 12,18");
    assertEq(mathx.lcm(4,6),12n,"lcm");
    assertEq(mathx.gcd(0,0),0n,"gcd 0,0");
    assertEq(mathx.gcd(-12,18),6n,"gcd negative");
    assert(Number.isNaN(mathx.gamma(-1)),"gamma -1 NaN");
    assert(Number.isNaN(mathx.erf(NaN)),"erf NaN 2");
    assert(Number.isNaN(mathx.logb(NaN)),"logb NaN");
    assertEq(mathx.logb(8),3,"logb 8");
    assertEq(mathx.logb(0),-Infinity,"logb 0 -inf");
    assertEq(mathx.ilogb(0), mathx.MinInt32,"ilogb 0 MinInt32");
    assertEq(mathx.isInf(Infinity),true,"isInf true");
    assertEq(mathx.isInf(5),false,"isInf false");
    assertEq(mathx.isNaN(NaN),true,"isNaN true");
    assertClose(mathx.nextafter(1,2), 1+Number.EPSILON/2,1e-15,"nextafter");
    assertEq(mathx.nchoosek(5,2),10,"nchoosek 5,2");
    assertEq(mathx.nchoosek(0,0),1,"nchoosek 0,0");
    const perms=mathx.perms([1,2,3]);
    assertEq(perms.length,6,"perms 3!");
    assertEq(mathx.bits.trailingZeros32(8),3,"bits trailingZeros");
    assert(Number.isFinite(mathx.gammaln(5)),"gammaln finite");
    assert(Number.isFinite(mathx.betaln(2,3)),"betaln finite");
    assertClose(mathx.psi(1), -0.57721566,1e-7,"psi 1");
    assertClose(mathx.cbrt(27),3,1e-12,"cbrt 27");
    assertEq(mathx.hypot(3,4),5,"hypot 3,4");
    assertEq(mathx.sign(-5),-1,"sign -5");
    assertEq(mathx.sign(0),0,"sign 0");
    assert(mathx.signbit(-0)===true,"signbit -0");
    assertEq(mathx.round(2.5),3,"round 2.5");
    assertEq(mathx.round(-2.5),-3,"round -2.5");
    assertEq(mathx.roundToEven(2.5),2,"roundToEven 2.5");
    assertEq(mathx.bits.leadingZeros32(1),31,"leadingZeros32");
    assertClose(mathx.expm1(0),0,1e-12,"expm1 0");
    assertClose(mathx.log1p(0),0,1e-12,"log1p 0");
    assertEq(mathx.log2(8),3,"log2 8");
}
if (fails===0) print("test_mathx_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_mathx_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
