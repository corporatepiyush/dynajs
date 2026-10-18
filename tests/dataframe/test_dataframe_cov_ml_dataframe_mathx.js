// flags: --std
// Split from tests/test_cov_ml_dataframe_mathx.js: the dataframe sections.
import { DataFrame } from "dyna:dataframe";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== dataframe ===");
{
    const df=new DataFrame({a:new Float64Array([1,2,3]), b:new Int32Array([4,5,6])});
    assertEq(df.ROWS,3,"df rows");
    assertEq(df.COLS,2,"df cols");
    assert(df.COLUMNS.includes("a") && df.COLUMNS.includes("b"),"df columns");
    const df2=df.FROM_RECORDS([{a:1,b:"x"},{a:2,b:"y"}]);
    assertEq(df2.ROWS,2,"from_records rows");
    assert(df2.COLUMNS.includes("a"),"from_records columns");
    await assertThrows(()=>new DataFrame({a:new Float64Array([1,2]), b:new Float64Array([1,2,3])}),"mismatched lengths throws");
    const copy=df.COPY();
    assertEq(copy.ROWS,3,"copy rows");
    const sel=df.SELECT(["a"]);
    assertEq(sel.COLS,1,"select cols");
    const dropped=df.DROP_COLUMNS(["b"]);
    assertEq(dropped.COLS,1,"drop cols");
    const renamed=df.RENAME({a:"aa"});
    assert(renamed.COLUMNS.includes("aa"),"rename");
    const sliced=df.SLICE(1,2);
    assertEq(sliced.ROWS,1,"slice rows");
    assert(df.SCHEMA().length===2,"schema len");
    assert(Object.keys(df.DTYPES()).length===2,"dtypes");
    assert(df.INFO().rows===3,"info rows");
    assert(df.MEMORY_USAGE().total>0,"memory usage");
}
{
    const n=2000;
    const keys=new Int32Array(n);
    const vals=new Float64Array(n);
    for(let i=0;i<n;i++){ keys[i]=i%500; vals[i]=1; }
    const df=new DataFrame({k:keys, v:vals});
    const g=df.GROUP_BY_SUM("k","v");
    assert(g.keys.length===500,"group_by_sum distinct 500");
    for(let i=0;i<g.values.length;i++) assertEq(g.values[i],4,"group_by_sum value 4");
    const gm=df.GROUP_BY_MEAN("k","v");
    assert(gm.values[0]===1,"group_by_mean 1");
    const gc=df.GROUP_BY_COUNT("k");
    assert(gc.values[0]===4,"group_by_count 4");
    const keys2=new Int32Array(n);
    const vals2=new Float64Array(n);
    for(let i=0;i<n;i++){ keys2[i]=i; vals2[i]=i; }
    const dfLarge=new DataFrame({k:keys2, v:vals2});
    const gLarge=dfLarge.GROUP_BY_SUM("k","v");
    assertEq(gLarge.keys.length,2000,"group_by_sum 2000 distinct");
    let found=false;
    for(let i=0;i<gLarge.keys.length;i++) if(gLarge.keys[i]===123) { assertEq(gLarge.values[i],123,"group_by_sum large key 123"); found=true; }
    assert(found,"found key 123");
}
{
    const L=new DataFrame({k:new Int32Array([1,2,3]), lv:new Float64Array([10,20,30])});
    const R=new DataFrame({k:new Int32Array([2,3,4]), rv:new Float64Array([200,300,400])});
    const inner=L.JOIN(R,"k","k");
    assert(inner.ROWS>=2,"join inner rows");
    assert(inner.COLUMNS.includes("k"),"join columns");
    const left=L.JOIN(R,"k","k","left");
    assert(left.ROWS===3,"join left rows");
    const right=L.JOIN(R,"k","k","right");
    assert(right.ROWS===3,"join right rows");
    const outer=L.JOIN(R,"k","k","outer");
    assert(outer.ROWS===4,"join outer rows");
    const dfP=new DataFrame({idx:new Int32Array([0,0,1,1]), col:new Int32Array([0,1,0,1]), val:new Float64Array([1,2,3,4])});
    const piv=dfP.PIVOT("idx","col","val","sum");
    assert(piv.ROWS===2,"pivot rows");
    const dfM=new DataFrame({a:new Float64Array([1,2]), b:new Float64Array([3,4]), c:new Float64Array([5,6])});
    const melted=dfM.MELT(["a"],["b","c"]);
    assert(melted.ROWS===4,"melt rows = 2*2");
    const t1=new DataFrame({t:new Int32Array([1,2,3]), v:new Float64Array([10,20,30])});
    const t2=new DataFrame({t:new Int32Array([1,2]), w:new Float64Array([100,200])});
    const asof=t1.ASOF_JOIN(t2,"t","t");
    assert(asof.ROWS===3,"asof rows");
    const R2=new DataFrame({k:new Int32Array([4,5]), lv:new Float64Array([400,500])});
    const c=L.CONCAT(R2);
    assert(c.ROWS===5,"concat rows");
    const timeDF=new DataFrame({t:new Float64Array([0,1,2,3,4,5]), v:new Float64Array([1,2,3,4,5,6])});
    const res=timeDF.RESAMPLE("t",2,"sum");
    assert(res.ROWS>0,"resample rows");
}
{
    const dfNa=new DataFrame({v:new Float64Array([1,NaN,2,NaN,3])});
    assertEq(dfNa.COUNT_NULLS("v"),2,"count nulls NaN");
    const sumNa=dfNa.SUM("v");
    assert(Number.isNaN(sumNa),"sum propagates NaN, got "+sumNa);
    const dfStats=new DataFrame({v:new Float64Array([1,2,3,4,5])});
    assertClose(dfStats.MEAN("v"),3,1e-12,"mean 3");
    assertClose(dfStats.VARIANCE("v"),2.5,1e-12,"variance sample");
    assertClose(dfStats.VARIANCE_POP("v"),2,1e-12,"variance pop");
    assertClose(dfStats.STDDEV("v"), Math.sqrt(2.5),1e-12,"stddev");
    assert(dfStats.MIN("v")===1 && dfStats.MAX("v")===5,"min max");
    assert(dfStats.MEDIAN("v")===3,"median");
    assert(dfStats.QUANTILE("v",0.5)===3,"quantile");
    const dfSort=new DataFrame({v:new Float64Array([3,NaN,1,2])});
    const sorted=dfSort.SORT("v");
    assert(sorted[sorted.length-1]!==sorted[sorted.length-1] || sorted[sorted.length-1]===sorted[sorted.length-1],"sort NaN last check");
    assert(Number.isNaN(sorted[sorted.length-1]),"sorted NaN last");
    const N=10000;
    const bigKeys=new Int32Array(N);
    const bigVals=new Float64Array(N);
    for(let i=0;i<N;i++){ bigKeys[i]=i%1000; bigVals[i]=i%10; }
    const bigDF=new DataFrame({k:bigKeys, v:bigVals});
    assertEq(bigDF.ROWS,10000,"big df 10k rows");
    const bigG=bigDF.GROUP_BY_SUM("k","v");
    assert(bigG.keys.length===1000,"big group 1000 distinct");
}
{
    const df=new DataFrame({v:new Float64Array([1,2,3]), w:new Int32Array([1,2,3])});
    assertClose(df.COV_SAMP("v","w"),1,1e-12,"cov_samp");
    assertClose(df.CORR("v","w"),1,1e-12,"corr");
    assertClose(df.DOT_PRODUCT("v","w"),14,1e-12,"dot_product");
    assert(df.BITWISE_AND("w")===0,"bitwise_and");
    assert(df.SUM_CHECKED("w")===6,"sum_checked");
    const mask=new Uint8Array([1,0,1]);
    const filtered=df.FILTER(mask);
    assertEq(filtered.ROWS,2,"filter rows");
    const isin=df.ISIN("v",[1,2]);
    assert(isin[0]===1 && isin[2]===0,"isin");
    const notNa=df.DROP_NA("v");
    assert(notNa[0]===1,"drop_na");
    const json=df.TO_JSON();
    assert(typeof json==="string" && json.length>0,"to_json");
    const csv=df.TO_CSV();
    assert(csv.includes("v"),"to_csv");
    const cols=df.TO_COLUMNS();
    assert(cols.v.length===3,"to_columns");
    const recs=df.TO_RECORDS();
    assert(recs.length===3,"to_records");
    assert(df.HEAD("v",2).length===2,"head");
    assert(df.TAIL("v",2).length===2,"tail");
    assert(df.FIRST("v")===1,"first");
    assert(df.LAST("v")===3,"last");
    assert(df.ARG_MIN("v")===0,"arg_min");
    assert(df.ARG_MAX("v")===2,"arg_max");
    const gc=df.GROUP_CONCAT("w",",");
    assert(typeof gc==="string","group_concat");
}
if (fails===0) print("test_dataframe_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_dataframe_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
