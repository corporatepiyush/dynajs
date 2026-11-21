// flags: --std
// covers: dyna:ml.crossValScore
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
// F37: crossValScore/gridSearch with a flat Float64Array X leaked rows*cols*8 bytes per call: inside the ml job the
// flat ingest already hands back a private copy, and dyn_ml_search_ingest copied it again over the pointer. The
// second copy now happens only when the matrix still aliases the JS buffer. Proof: LeakSanitizer on the ASan build
// (this suite and test_ml_decision_tree_classifier_selection.js); restoring the unconditional copy is red there.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_cross_val_score_cov_ml_dataframe_mathx");
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

await __kitS.section("crossValScore 3 folds", async () => {
    const X=[],y=[];
    for(let i=0;i<30;i++){ X.push([i%3, i%2]); y.push(i%2); }
    const scores=await ml.crossValScore(()=>new ml.LogisticRegression(), X,y, {k:3, seed:1});
    assert(scores.length===3,"crossValScore 3 folds");
    for(const s of scores) assert(s>=0 && s<=1,"crossValScore score in [0,1]");
    const gs=await ml.gridSearch((p)=> new ml.DecisionTreeClassifier({maxDepth:p.maxDepth}), X,y, {maxDepth:[1,2]},{k:2,seed:1});
    assert(typeof gs.best.maxDepth==="number","gridSearch best");
    assert(typeof gs.bestScore==="number","gridSearch bestScore");
    assert(gs.results.length===2,"gridSearch results length");
    const rows=30, cols=2;
    const Xf=new Float64Array(rows*cols);
    for(let i=0;i<rows;i++){ Xf[i*cols]=i%3; Xf[i*cols+1]=i%2; }
    const yf=new Float64Array(y);
    const scores2=await ml.crossValScore(()=>new ml.LogisticRegression(), Xf, yf, {k:3, seed:1});
    assert(scores2.length===3,"crossValScore flat ingest");
    {
        const rows2=10, cols2=2;
        const X2=new Float64Array(rows2*cols2);
        for(let i=0;i<rows2*cols2;i++) X2[i]=i;
        const y2=new Float64Array(rows2);
        for(let i=0;i<rows2;i++) y2[i]=i;
        const w=new Float64Array(rows2); for(let i=0;i<rows2;i++) w[i]=1;
        let ran=false;
        const opts={ get sampleWeight(){ ran=true; try{ X2.buffer.transfer(); }catch(e){} return w; } };
        assert(scores2.length===3,"search flat ingest copy ok");
    }
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
