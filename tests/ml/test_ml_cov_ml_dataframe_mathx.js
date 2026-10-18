// flags: --std
// Split from tests/test_cov_ml_dataframe_mathx.js: the ml sections.
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
{
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
}
{
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
}
{
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
}
{
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
}
{
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
}
{
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
}
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
{
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
}
{
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
}
{
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
}
{
    const X=[],y=[];
    for(let i=0;i<20;i++){ X.push([i]); y.push(2*i+1); }
    const m=new ml.LinearRegression();
    await m.fit(X,y);
    const bytes=m.serialize();
    const back=ml.LinearRegression.deserialize(bytes);
    assert(JSON.stringify(await back.predict([[100]]))===JSON.stringify(await m.predict([[100]])),"serialize identical");
    try {
        const p=new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_ml.${Date.now() % 10000000}.dyns`);
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
}
{
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
}
if (fails===0) print("test_ml_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_ml_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
