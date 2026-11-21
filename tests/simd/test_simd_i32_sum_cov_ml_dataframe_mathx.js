// covers: dyna:simd.i32Sum
// Split from tests/test_cov_ml_dataframe_mathx.js: the simd sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_sum_cov_ml_dataframe_mathx");
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
    

__kitS.section("simd i32Sum", () => {
{
        const a=new Int32Array([1,2,3,4]);
        assertEq(simd.i32Sum(a),10,"simd i32Sum");
        assertEq(simd.i32Min(a),1,"simd i32Min");
        assertEq(simd.i32Max(a),4,"simd i32Max");
        assertEq(simd.i32Dot(a,a),30,"simd i32Dot");
        const out=new Int32Array(4); simd.i32Add(out, a, a); assertEq(out[0],2,"simd i32Add");
    }
if (fails===0) print("test_simd_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_simd_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
