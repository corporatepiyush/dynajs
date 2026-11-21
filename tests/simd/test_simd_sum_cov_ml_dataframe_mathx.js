// covers: dyna:simd.sum
// Split from tests/test_cov_ml_dataframe_mathx.js: the simd sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_sum_cov_ml_dataframe_mathx");
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
    

__kitS.section("simd sum NaN propagates", () => {
{
        const a=f32([1, NaN, Infinity, -Infinity, 0]);
        assert(Number.isNaN(simd.sum(a)),"simd sum NaN propagates");
        const mx=simd.max(f32([1,2,3]));
        assertEq(mx,3,"simd max no NaN");
        const b=f32([1,1,1,1,1]);
        assert(Number.isNaN(simd.dot(a,b)),"simd dot NaN");
        const c=f32([3,4]);
        assertClose(simd.normL2(c),5,1e-4,"simd normL2 3,4");
        assertClose(simd.normL1(c),7,1e-4,"simd normL1");
        const d=f32([1,2,3]); simd.scale(d,2); assertEq(d[0],2,"simd scale");
        const e=f32([1,1]); const x=f32([1,1]); simd.axpy(e,2,x); assertEq(e[0],3,"simd axpy");
        const f=f32([1,2]); simd.clamp(f,0,1); assertEq(f[1],1,"simd clamp 2->1");
        const g=f32([-1,0,1]); simd.threshold(g,0); assertEq(g[0],0,"simd threshold -1=>0"); assertEq(g[2],1,"simd threshold 1=>1");
        const vals=f32([5,1,3,2,4]);
        const idx=simd.topkIndices(vals,2);
        assertEq(idx.length,2,"simd topk 2");
        const p=f32([0,0]), q=f32([3,4]);
        assertClose(simd.distL2(p,q),5,1e-4,"simd distL2");
        assertClose(simd.distL1(p,q),7,1e-4,"simd distL1");
    }
if (fails===0) print("test_simd_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_simd_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
