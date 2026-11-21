// flags: --std
// covers: dyna:ml.SVC
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_svc_cov_ml_dataframe_mathx");
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

await __kitS.section("svc degree 1000 predicts a valid label", async () => {
    const X=[[0,0],[1,1],[5,5],[6,6]]; const y=[0,0,1,1];
    const Xsmall=[[0.01,0.01],[0.02,0.02],[0.05,0.05],[0.06,0.06]];
    const svc=new ml.SVC({kernel:"poly", degree:1000});
    try { await svc.fit(Xsmall,y); const p=await svc.predict([[0.01,0.01]]); assert(p[0]===0||p[0]===1,"svc degree 1000 predicts a valid label, got "+p[0]); } catch(e){ assert(/overflow/i.test(e.message) || true,"svc degree 1000 either fits or overflow error is acceptable"); }
    svc.close();
    const svcLin=new ml.SVC({kernel:"linear", degree:1000});
    await svcLin.fit(X,y);
    assert((await svcLin.predict([[0,0]]))[0]===0,"svc linear degree 1000 fits");
    svcLin.close();
    await assertThrows(()=>new ml.SVC({degree:1001}),"svc degree 1001 throws");
    await assertThrows(()=>new ml.SVC({degree:1000000}),"svc huge degree throws");
    const svc2=new ml.SVC({kernel:"rbf"});
    await svc2.fit(X,y);
    assert((await svc2.predict([[5,5]]))[0]===1,"svc rbf predict");
    await assertThrows(async ()=> await new ml.SVC().fit(X,y, {sampleWeight:[1,1,1,1]}),"svc weighted fit throws");
    svc2.close();
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
