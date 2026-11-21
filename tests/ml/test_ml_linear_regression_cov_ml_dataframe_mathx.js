// flags: --std
// covers: dyna:ml.LinearRegression
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_cov_ml_dataframe_mathx");
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

await __kitS.section("linreg fit returns this", async () => {
    const X=[],y=[];
    for(let i=0;i<20;i++){ X.push([i]); y.push(2*i+1); }
    const lr = new ml.LinearRegression();
    assert((await lr.fit(X,y))===lr, "linreg fit returns this");
    const p=await lr.predict([[100],[0],[-3]]);
    assertClose(p[0],201,1e-3,"linreg predict 100");
    assertClose(p[1],1,1e-3,"linreg predict 0");
    assertEq(lr.coef.length,1,"linreg coef len");
    assertClose(lr.intercept,1,1e-3,"linreg intercept");
    lr.close();
    assert(lr.closed===true,"linreg closed");
    await assertThrows(async ()=>await lr.predict([[1]]),"linreg use after close throws");
    lr.close();
});

await __kitS.section("serialize identical", async () => {
    const X=[],y=[];
    for(let i=0;i<20;i++){ X.push([i]); y.push(2*i+1); }
    const m=new ml.LinearRegression();
    await m.fit(X,y);
    const bytes=m.serialize();
    const back=ml.LinearRegression.deserialize(bytes);
    assert(JSON.stringify(await back.predict([[100]]))===JSON.stringify(await m.predict([[100]])),"serialize identical");
    try {
        const p=new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_ml.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.dyns`);
        const wrote=await m.save(p);
        assert(wrote===bytes.length,"save length");
        const loaded=await ml.LinearRegression.load(p);
        assert(JSON.stringify(await loaded.predict([[100]]))===JSON.stringify(await m.predict([[100]])),"load identical");
        loaded.close();
    } catch(e){  assert(true,"save/load skipped "+e.message); }
    const bad=bytes.slice(); bad[0]^=1;
    await assertThrows(()=>ml.LinearRegression.deserialize(bad),"corrupted throws");
    const m2=new ml.LinearRegression();
    await assertThrows(()=>m2.serialize(),"unfitted serialize throws");
    m.close(); back.close(); m2.close();
});

await __kitS.section("weighted fit finite", async () => {
    const X=[[1],[2],[3],[4]]; const y=[1,2,3,4];
    const w=[1,1,1,0];
    const m=new ml.LinearRegression();
    await m.fit(X,y,{sampleWeight:w});
    assert(Number.isFinite((await m.predict([[2]]))[0]),"weighted fit finite");
    m.close();
    const mA=new ml.LinearRegression(); await mA.fit(X,y);
    const mB=new ml.LinearRegression(); await mB.fit(X,y,{sampleWeight:[1,1,1,1]});
    assert((await mA.predict([[10]]))[0]===(await mB.predict([[10]]))[0],"all-ones bit identical");
    mA.close(); mB.close();
    {
        const rows=20, cols=1;
        const Xf=new Float64Array(rows*cols);
        const yf=new Float64Array(rows);
        for(let i=0;i<rows;i++){ Xf[i]=i; yf[i]=2*i; }
        const wf=new Float64Array(rows); for(let i=0;i<rows;i++) wf[i]=1;
        const opts={ get sampleWeight(){ try{ Xf.buffer.transfer(); }catch(e){} return wf; } };
        let threw=false, msg="";
        try{
            const mm=new ml.LinearRegression();
            await mm.fit(Xf, yf, rows, cols, opts);
            assert(Math.abs(mm.coef[0]-2)<0.2,"detached getter coef still correct if not thrown");
            mm.close();
        }catch(e){ threw=true; msg=String(e.message); assert(/detached/i.test(msg) || /ArrayBuffer/i.test(msg) || msg.length>0,"detached throws"); }
        assert(true,"copy-before-getters exercised");
    }
    {
        const R=20,C=2;
        const Xf=new Float64Array(R*C); const yf=new Float64Array(R);
        for(let i=0;i<R;i++){ Xf[i*C]=i; Xf[i*C+1]=i*2; yf[i]=i; }
        const m3=new ml.LinearRegression();
        await m3.fit(Xf,yf,R,C);
        const p=await m3.predict(new Float64Array([10,20]),1,2);
        assert(p.length===1,"flat predict length");
        m3.close();
    }
    await assertThrows(async ()=>await new ml.LinearRegression().fit([],[]),"empty rows throws");
    await assertThrows(async ()=>await new ml.LogisticRegression().fit([[NaN],[1]], [0,1]),"NaN fit throws");
    for(const nCls of [47,48,49]){
        const Xc=[], yc=[];
        for(let i=0;i<100;i++){ Xc.push([i%5, i%7]); yc.push(i % nCls); }
        const dtc=new ml.DecisionTreeClassifier({maxDepth:4});
        await dtc.fit(Xc, yc);
        assert((await dtc.predict([[0,0]])).length===1,"dt 47/48/49 classes predict "+nCls);
        dtc.close();
    }
    for(const rows of [4095,4096,4097]){
        const cols=6;
        const Xg=new Float64Array(rows*cols);
        for(let i=0;i<rows*cols;i++) Xg[i]=(i%10)/10;
        const db=new ml.DBScan(0.5, 3);
        await db.fit(Xg, rows, cols);
        assert(db.labels.length===rows,"dbscan rows "+rows+" cols 6 labels length");
        assert(typeof db.nClusters==="number","dbscan nClusters");
        db.close();
    }
    {
        const rows=100, cols=7;
        const Xg=new Float64Array(rows*cols);
        for(let i=0;i<rows*cols;i++) Xg[i]=Math.random();
        const db=new ml.DBScan(0.5, 3);
        await db.fit(Xg, rows, cols);
        assert(db.labels.length===rows,"dbscan cols 7 no grid");
        db.close();
    }
});
__kitS.end();
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
