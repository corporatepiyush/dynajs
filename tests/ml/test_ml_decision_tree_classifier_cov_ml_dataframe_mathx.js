// flags: --std
// covers: dyna:ml.DecisionTreeClassifier
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_classifier_cov_ml_dataframe_mathx");
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

await __kitS.section("dt predict length", async () => {
    const X=[],y=[];
    for(let i=0;i<40;i++){ X.push([i%2, i%3]); y.push(i%2); }
    const dt=new ml.DecisionTreeClassifier({maxDepth:3});
    await dt.fit(X,y);
    const pr=await dt.predict([[0,0],[1,1]]);
    assert(pr.length===2,"dt predict length");
    assert(Array.isArray(dt.featureImportances),"dt featureImportances array");
    assert(typeof dt.depth==="number","dt depth number");
    const proba=await dt.predictProba([[0,0]]);
    assert(proba[0].length===2 && Math.abs(proba[0][0]+proba[0][1]-1)<1e-12,"dt proba sum 1");
    const apply=await dt.apply([[0,0]]);
    assert(apply.length===1,"dt apply length");
    const bytes=dt.serialize();
    assert(bytes instanceof Uint8Array,"dt serialize Uint8Array");
    const back=ml.DecisionTreeClassifier.deserialize(bytes);
    assert(JSON.stringify(await back.predict([[0,0],[1,1]]))===JSON.stringify(pr),"dt deserialize identical");
    dt.close(); back.close();
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
