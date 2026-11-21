// flags: --std
// covers: dyna:ml.RandomForestClassifier
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_cov_ml_dataframe_mathx");
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

await __kitS.section("rf predict length", async () => {
    const X=[],y=[];
    for(let i=0;i<50;i++){ X.push([i%4, i%5]); y.push(i%2); }
    const rf=new ml.RandomForestClassifier({nEstimators:5, seed:1, maxDepth:4});
    await rf.fit(X,y);
    assert((await rf.predict(X)).length===50,"rf predict length");
    assert(rf.featureImportances.length===2,"rf featureImportances");
    const proba=await rf.predictProba([[0,0]]);
    assert(proba[0].length===2,"rf proba");
    const bytes=rf.serialize();
    const back=ml.RandomForestClassifier.deserialize(bytes);
    assert(JSON.stringify(await back.predict([[0,0]]))===JSON.stringify(await rf.predict([[0,0]])),"rf roundtrip");
    rf.close(); back.close();
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
