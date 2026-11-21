// flags: --std
// covers: dyna:ml.StandardScaler
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_standard_scaler_cov_ml_dataframe_mathx");
import * as std from "std";
import * as ml from "dyna:ml";
import { Path } from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");

await __kitS.section("scaler mean", async () => {
    const X=[[1,10],[2,20],[3,30]];
    const ss=new ml.StandardScaler();
    await ss.fit(X);
    assertClose(ss.mean[0],2,1e-9,"scaler mean");
    assert(ss.std.length===2,"scaler std len");
    const Xt=await ss.transform([[1,10]]);
    assertClose(Xt[0][0], -1.224744871391589, 1e-6,"scaler transform sklearn ddof=0");
    const inv=await ss.inverseTransform(Xt);
    assertClose(inv[0][0],1,1e-9,"scaler inverse");
    const Xc=[[5],[5],[5]];
    const ssc=new ml.StandardScaler();
    await ssc.fit(Xc);
    assertEq(ssc.std[0],1,"scaler constant std 1.0");
    ssc.close(); ss.close();
    const mm=new ml.MinMaxScaler();
    await mm.fit(X);
    assertEq(mm.dataMin.length,2,"minmax dataMin len");
    const Xm=await mm.transform([[2,20]]);
    assert(Xm[0][0]>=0 && Xm[0][0]<=1,"minmax transform 0-1");
    const inv2=await mm.inverseTransform(Xm);
    assertClose(inv2[0][0],2,1e-9,"minmax inverse");
    mm.close();
    await assertThrows(async ()=>await new ml.MinMaxScaler().fit(X, {sampleWeight:[1,1,1]}),"minmax weighted throws");
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
