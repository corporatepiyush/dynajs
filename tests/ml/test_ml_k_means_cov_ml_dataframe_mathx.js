// flags: --std
// covers: dyna:ml.KMeans
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_means_cov_ml_dataframe_mathx");
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

await __kitS.section("kmeans clusters differ", async () => {
    const km=new ml.KMeans(2,7);
    const X=[[0,0],[0.2,-0.1],[-0.1,0.2],[10,10],[10.2,9.9],[9.8,10.1]];
    await km.fit(X);
    const labs=await km.predict(X);
    assert(labs[0]!==labs[3],"kmeans clusters differ");
    assert(typeof km.inertia==="number" && km.inertia>=0,"kmeans inertia");
    await assertThrows(async ()=>await km.predict([[NaN]]),"kmeans predict NaN throws");
    km.close();
    const km2=new ml.KMeans(5);
    await assertThrows(async ()=>await km2.fit([[1],[2]]),"kmeans fewer rows than clusters throws");
    km2.close();
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
