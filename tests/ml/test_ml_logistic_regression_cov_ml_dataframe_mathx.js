// flags: --std
// covers: dyna:ml.LogisticRegression
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_cov_ml_dataframe_mathx");
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

await __kitS.section("logreg separable", async () => {
    const X=[],y=[];
    const c0=[[0,0],[1,0],[0,1],[1,1]]; const c1=[[5,5],[4,5],[5,4],[4,4]];
    for(const p of c0){ X.push(p); y.push(0);} for(const p of c1){ X.push(p); y.push(1);}
    const lg=new ml.LogisticRegression();
    await lg.fit(X,y);
    const labels=await lg.predict([[0,0],[5,5]]);
    assert(labels[0]===0 && labels[1]===1,"logreg separable");
    const proba=await lg.predictProba([[0,0],[5,5]]);
    assert(proba.length===2 && proba[0].length===2,"logreg proba shape");
    assert(Math.abs(proba[0][0]+proba[0][1]-1)<1e-12,"logreg proba sum 1");
    assert(proba[0][1]<0.5 && proba[1][1]>0.5,"logreg proba ordering");
    assert(Array.isArray(lg.classes) && lg.classes.length===2,"logreg classes");
    assert(typeof lg.converged==="boolean","logreg converged bool");
    lg.close();
});

await __kitS.section("pipeline fitted", async () => {
    const X=[],y=[];
    for(let i=0;i<30;i++){ X.push([i%3, i%2]); y.push(i%2); }
    const pipe=new ml.Pipeline([new ml.StandardScaler(), new ml.LogisticRegression()]);
    await pipe.fit(X,y);
    assert(pipe.fitted===true,"pipeline fitted");
    assert(pipe.length===2,"pipeline length");
    const pred=await pipe.predict([[0,0]]);
    assert(pred.length===1,"pipeline predict");
    const stage0=pipe.stage(0);
    assert(stage0 instanceof ml.StandardScaler,"pipeline stage 0");
    assert(pipe.stage(-1) instanceof ml.LogisticRegression,"pipeline stage -1");
    assert(pipe.estimator instanceof ml.LogisticRegression,"pipeline estimator");
    const trans=await pipe.transform([[0,0]]);
    assert(trans.length===1,"pipeline transform");
    const proba=await pipe.predictProba([[0,0]]);
    assert(proba[0].length===2,"pipeline predictProba");
    pipe.close();
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
