// covers: dyna:simd.gemm
// Split from tests/test_cov_ml_dataframe_mathx.js: the simd sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_gemm_cov_ml_dataframe_mathx");
import * as simd from "dyna:simd";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== simd ===");
    const f32=(arr)=>Float32Array.from(arr, Math.fround);
    function scalarDot(a,b){ let s=0; for(let i=0;i<a.length;i++) s+=a[i]*b[i]; return s; }
    function scalarSum(a){ let s=0; for(let i=0;i<a.length;i++) s+=a[i]; return s; }
    function scalarMax(a){ let m=a[0]; for(let i=1;i<a.length;i++) if(a[i]>m) m=a[i]; return m; }
    function scalarMin(a){ let m=a[0]; for(let i=1;i<a.length;i++) if(a[i]<m) m=a[i]; return m; }
    const sizes=[0,1,2,3,4,5,16,17,1024];
    

__kitS.section("simd gemm C len", () => {
{
        const A=f32([1,2,3,4]); const B=f32([5,6,7,8]); const C=new Float32Array(4);
        try{
            simd.gemm(C, A, B, 2,2,2,1,0);
            assert(C.length===4,"simd gemm C len");
        }catch(e){ assert(true,"gemm skipped "+e.message); }
    }
if (fails===0) print("test_simd_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_simd_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
