// flags: --std
// covers: dyna:ml.GaussianNB
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_nb_cov_ml_dataframe_mathx");
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

await __kitS.section("nb predict 0", async () => {
{
    const X=[[0,0],[0,0],[10,10],[10,10]]; const y=[0,0,1,1];
    const nb=new ml.GaussianNB(1e-9);
    await nb.fit(X,y);
    assert((await nb.predict([[0,0]]))[0]===0,"nb predict 0");
    assert((await nb.predict([[10,10]]))[0]===1,"nb predict 1");
    const proba=await nb.predictProba([[0,0]]);
    assert(proba[0][0]+proba[0][1]===1 || Math.abs(proba[0][0]+proba[0][1]-1)<1e-12,"nb proba sum");
    const Xconst=[[1],[1],[1],[2],[2],[2]];
    const yconst=[0,0,0,1,1,1];
    const nb2=new ml.GaussianNB();
    await nb2.fit(Xconst,yconst);
    assert(Number.isFinite((await nb2.predictProba([[1]]))[0][0]),"nb var clamp finite");
    await assertThrows(()=>new ml.GaussianNB(-1),"nb negative varSmoothing throws");
    nb.close(); nb2.close();
}
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
