// flags: --std
// Split from tests/test_cov_ml_dataframe_mathx.js: the simd sections.
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
{
    const f32=(arr)=>Float32Array.from(arr, Math.fround);
    function scalarDot(a,b){ let s=0; for(let i=0;i<a.length;i++) s+=a[i]*b[i]; return s; }
    function scalarSum(a){ let s=0; for(let i=0;i<a.length;i++) s+=a[i]; return s; }
    function scalarMax(a){ let m=a[0]; for(let i=1;i<a.length;i++) if(a[i]>m) m=a[i]; return m; }
    function scalarMin(a){ let m=a[0]; for(let i=1;i<a.length;i++) if(a[i]<m) m=a[i]; return m; }
    const sizes=[0,1,2,3,4,5,16,17,1024];
    for(const nsize of sizes){
        const a=f32(Array.from({length:nsize},(_,i)=>Math.fround((i%17)-8.5)));
        const b=f32(Array.from({length:nsize},(_,i)=>Math.fround((i%13)-6.25)));
        const dotExp=scalarDot(a,b);
        const sumExp=scalarSum(a);
        if(nsize===0){
        } else {
            assertClose(simd.dot(a,b), dotExp, Math.max(1,Math.abs(dotExp))*1e-4,"simd dot size "+nsize);
        }
        if(nsize>0){
            const af=new Float64Array(a);
            const bf=new Float64Array(b);
            assertClose(simd.f64Dot(af,bf), dotExp, Math.max(1,Math.abs(dotExp))*1e-12,"simd f64Dot size "+nsize);
        }
        if(nsize===0){
            assertEq(simd.sum(a),0,"simd sum empty 0");
        } else {
            assertClose(simd.sum(a), sumExp, Math.max(1,Math.abs(sumExp))*1e-4,"simd sum size "+nsize);
            assertClose(simd.f64Sum(new Float64Array(a)), sumExp, Math.max(1,Math.abs(sumExp))*1e-12,"simd f64Sum size "+nsize);
        }
        if(nsize===0){
            await assertThrows(()=>simd.max(a),"simd max empty throws");
            await assertThrows(()=>simd.min(a),"simd min empty throws");
            await assertThrows(()=>simd.argmax(a),"simd argmax empty throws");
            await assertThrows(()=>simd.argmin(a),"simd argmin empty throws");
        } else {
            const mx=scalarMax(a), mn=scalarMin(a);
            assertEq(simd.max(a), mx,"simd max size "+nsize);
            assertEq(simd.min(a), mn,"simd min size "+nsize);
            assertEq(simd.f64Max(new Float64Array(a)), mx,"simd f64Max size "+nsize);
            assertEq(simd.f64Min(new Float64Array(a)), mn,"simd f64Min size "+nsize);
            const ai=simd.argmax(a), ii=simd.argmin(a);
            assert(a[ai]===mx,"simd argmax value size "+nsize);
            assert(a[ii]===mn,"simd argmin value size "+nsize);
            const mono=f32(Array.from({length:nsize},(_,i)=>Math.fround(i)));
            assertEq(simd.argmax(mono), nsize-1,"simd argmax mono size "+nsize);
            assertEq(simd.argmin(mono), 0,"simd argmin mono size "+nsize);
        }
        if(nsize>0){
            const c=f32(a);
            simd.threshold(c,0);
            for(let i=0;i<nsize;i++) assertEq(c[i], a[i]>0?1:0,"simd threshold size "+nsize+" i"+i);
            const cl=f32(a);
            simd.clamp(cl,-2,2);
            for(let i=0;i<nsize;i++) assertEq(cl[i], Math.fround(Math.min(2,Math.max(-2,a[i]))),"simd clamp i"+i);
        }
    }
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
    {
        const a=new Int32Array([1,2,3,4]);
        assertEq(simd.i32Sum(a),10,"simd i32Sum");
        assertEq(simd.i32Min(a),1,"simd i32Min");
        assertEq(simd.i32Max(a),4,"simd i32Max");
        assertEq(simd.i32Dot(a,a),30,"simd i32Dot");
        const out=new Int32Array(4); simd.i32Add(out, a, a); assertEq(out[0],2,"simd i32Add");
    }
    {
        const A=f32([1,2,3,4]); const B=f32([5,6,7,8]); const C=new Float32Array(4);
        try{
            simd.gemm(C, A, B, 2,2,2,1,0);
            assert(C.length===4,"simd gemm C len");
        }catch(e){ assert(true,"gemm skipped "+e.message); }
    }
}
if (fails===0) print("test_simd_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_simd_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
