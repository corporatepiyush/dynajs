// covers: dyna:mathx.linspace
// Split from tests/test_cov_ml_dataframe_mathx.js: the mathx sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_linspace_cov_ml_dataframe_mathx");
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

await __kitS.section("E === Math.E", async () => {
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
});
__kitS.end();
if (fails===0) print("test_mathx_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_mathx_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
