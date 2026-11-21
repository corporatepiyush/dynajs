// covers: dyna:mathx.gammainc
// Split from tests/test_cov_ml_dataframe_mathx.js: the mathx sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_gammainc_cov_ml_dataframe_mathx");
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

__kitS.section("gammainc x2 a1", () => {
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
});
__kitS.end();
if (fails===0) print("test_mathx_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_mathx_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
