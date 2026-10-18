// flags: --std
// Split from tests/test_cov_ml_dataframe_mathx.js: the decimal sections.
import { Decimal, Money } from "dyna:decimal";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== decimal ===");
{
    const D=(x)=>new Decimal(x);
    assertEq(D("0").toString(),"0","decimal zero");
    assertEq(D("-0").toString(),"0","decimal -0 is 0");
    assertEq(D("1.500").toString(),"1.5","trailing zeros not part");
    assertEq(D("1.5").add(D("0.2")).toString(),"1.7","add");
    assertEq(D("5").sub(D("2")).toString(),"3","sub");
    assertEq(D("2").mul(D("3")).toString(),"6","mul");
    assertEq(D("10").div(D("4")).toString(),"2.5","div exact");
    assertEq(D("1").div(D("3")).toString(),"0."+"3".repeat(34),"div 1/3 34 digits");
    assertEq(D("10").div(D("3"),{precision:5}).toString(),"3.3333","div precision 5");
    assertEq(D("7").mod(D("3")).toString(),"1","mod");
    assertEq(D("-7").mod(D("3")).toString(),"-1","mod negative");
    assertEq(D("2").pow(10).toString(),"1024","pow 10");
    assertEq(D("2").pow(0).toString(),"1","pow 0");
    assertEq(D("2").pow(-1).toString(),"0.5","pow -1");
    await assertThrows(()=>D("2").pow(10001),"pow huge throws");
    await assertThrows(()=>D("2").pow(-10001),"pow huge negative throws");
    assertEq(D("3.14159").round(2).toString(),"3.14","round 2");
    assertEq(D("2.5").round(0,"halfEven").toString(),"2","halfEven 2.5->2");
    assertEq(D("3.5").round(0,"halfEven").toString(),"4","halfEven 3.5->4");
    assertEq(D("2.5").round(0,"halfUp").toString(),"3","halfUp 2.5->3");
    assertEq(D("0.5").round(0,"down").toString(),"0","down 0.5->0");
    assertEq(D("0.5").round(0,"up").toString(),"1","up 0.5->1");
    assertEq(D("0.5").round(0,"ceil").toString(),"1","ceil 0.5->1");
    assertEq(D("-0.5").round(0,"ceil").toString(),"0","ceil -0.5->0");
    assertEq(D("-0.5").round(0,"floor").toString(),"-1","floor -0.5->-1");
    assertEq(D("2.5").round(0,"halfOdd").toString(),"3","halfOdd 2.5->3");
    assertEq(D("1.5").round(0,"halfEven").toString(),"2","halfEven 1.5->2");
    assert(D("1.50").equals(D("1.5")),"equals 1.50==1.5");
    assertEq(D("1.50").cmp(D("1.5")),0,"cmp 1.50==1.5");
    assertEq(D("1.5").cmp(D("2")),-1,"cmp less");
    assertEq(D("2").cmp(D("1.5")),1,"cmp greater");
    assertEq(D("1.00").isZero(),false,"isZero false for 1");
    assertEq(D("0").isZero(),true,"isZero true");
    assertEq(D("12.34").toNumber(),12.34,"toNumber");
    await assertThrows(()=>D("1").div(D("0")),"div by zero throws");
    await assertThrows(()=>D("0").div(D("0")),"0/0 also throws");
    for(const bad of ["","abc","1.2.3","1e","Infinity","NaN"]) await assertThrows(()=>D(bad),"decimal refuses "+JSON.stringify(bad));
    await assertThrows(()=>D(NaN),"decimal refuses NaN number");
    await assertThrows(()=>D(Infinity),"decimal refuses Infinity");
    const s33="1."+"1".repeat(32);
    const d33=D("9".repeat(33));
    assertEq(d33.digits(),33,"digits 33");
    const d34=D("9".repeat(34));
    assertEq(d34.digits(),34,"digits 34");
    const d35=D("9".repeat(35));
    assertEq(d35.digits(),35,"digits 35");
    assertEq(D("9".repeat(33)).add(D("1")).toString(),"1"+"0".repeat(33),"carry 33");
    assertEq(D("9".repeat(34)).add(D("1")).toString(),"1"+"0".repeat(34),"carry 34");
    assertEq(D("9".repeat(35)).add(D("1")).toString(),"1"+"0".repeat(35),"carry 35");
    const m=new Money(100,"USD");
    const parts=m.allocate([1,1,1]);
    assert(parts.length===3,"money allocate 3 parts");
    assert(parts[0].amount()+parts[1].amount()+parts[2].amount()===100,"money allocate sum 100");
    assert(JSON.stringify(m.allocate([1,1,1]).map(x=>x.amount()))==="[34,33,33]","money allocate remainder earliest");
    const m2=new Money(5,"USD");
    assert(JSON.stringify(m2.allocate([70,30]).map(x=>x.amount()))==="[4,1]","money allocate weighted");
    await assertThrows(()=>new Money(100,"USD").allocate([]),"money allocate empty throws");
    await assertThrows(()=>new Money(100,"USD").allocate([0,0]),"money allocate zero sum throws");
    await assertThrows(()=>new Money(100,"USD").allocate([1,-1]),"money allocate negative throws");
    await assertThrows(()=>new Money(100,"USD").allocate([1.5]),"money allocate fractional throws");
    const w62=Math.pow(2,62);
    await assertThrows(()=>new Money(100,"USD").allocate([w62,w62,w62,w62]),"money allocate wrap throws");
    await assertThrows(()=>new Money(1,"USD").allocate([w62,w62,w62]),"money allocate wrap nonzero throws");
    const huge="9".repeat(100000);
    await assertThrows(()=>D(huge).div(D("1")),"huge 100k digits throws or cap");
    await assertThrows(()=>D("9".repeat(100001)),"100001 digits throws");
    const dh=D("9".repeat(5000));
    assert(dh.digits()===5000,"huge 5000 digits ok");
}
if (fails===0) print("test_decimal_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_decimal_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
