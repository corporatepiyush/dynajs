// covers: dyna:time.RRule
// Split from tests/test_cov_ml_dataframe_mathx.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_cov_ml_dataframe_mathx");
import { RRule, Duration, PlainDate, PlainDateTime, PlainTime, Format, parseDuration, durationString, formatRFC3339, parseRFC3339, date, fromUnix } from "dyna:time";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== time RRule ===");
    function E(y,m,d,h=0,mi=0,s=0){ return Date.UTC(y,m-1,d,h,mi,s)/1000; }
    function secs(dates){ return dates.map(d=>d.getTime()/1000); }
    

await __kitS.section("SECONDLY huge window throws budget", async () => {
{
        const sec=new RRule({freq:"SECONDLY", dtstart:E(2020,1,1)});
        await assertThrows(()=>sec.between(E(2020,1,1), E(2022,1,1)),"SECONDLY huge window throws budget");
        const ten=sec.between(E(2020,1,1), E(2020,1,1,0,0,10), true);
        assertEq(ten.length,11,"SECONDLY in-budget 11");
        const cnt=new RRule({freq:"SECONDLY", dtstart:E(2020,1,1), count:1e9});
        await assertThrows(()=>cnt.next(E(2021,3,1)),"SECONDLY next far beyond budget throws");
        await assertThrows(()=>cnt.prev(E(2021,3,1)),"SECONDLY prev far beyond budget throws");
        const inf=new RRule({freq:"DAILY", dtstart:E(2020,1,1)});
        await assertThrows(()=>inf.all(),"infinite all throws");
        assertEq(inf.all(3).length,3,"infinite all(3)");
    }
});

await __kitS.section("wkst number vs string same", async () => {
{
        const r1=new RRule({freq:"WEEKLY", dtstart:E(2026,1,5), byweekday:["MO"], wkst:"MO", count:2});
        const r2=new RRule({freq:"WEEKLY", dtstart:E(2026,1,5), byweekday:["MO"], wkst:1, count:2});
        assert(JSON.stringify(secs(r1.all()))===JSON.stringify(secs(r2.all())),"wkst number vs string same");
        const rSU=new RRule({freq:"WEEKLY", dtstart:E(2026,1,5), byweekday:["SU"], wkst:"SU", count:2});
        assert(rSU.all().length===2,"wkst SU");
        await assertThrows(()=>new RRule({freq:"WEEKLY", wkst:99}),"wkst invalid throws");
    }
});

__kitS.section("count 3", () => {
{
        const r=new RRule({freq:"DAILY", dtstart:E(2026,1,1), count:3});
        assertEq(secs(r.all()).length,3,"count 3");
        assertEq(secs(r.all())[0], E(2026,1,1),"dtstart");
        const rUntil=new RRule({freq:"DAILY", dtstart:E(2026,1,1), until:E(2026,1,3)});
        assert(rUntil.all().length===3,"until inclusive");
        const rInterval=new RRule({freq:"DAILY", interval:2, dtstart:E(2026,1,1), count:3});
        assertEq(secs(rInterval.all())[1], E(2026,1,3),"interval 2");
        const rByMonth=new RRule({freq:"YEARLY", bymonth:[2], bymonthday:[15], dtstart:E(2020,1,1), count:3});
        assertEq(secs(rByMonth.all()).length,3,"by month");
        const rByWeekday=new RRule({freq:"MONTHLY", byweekday:["MO"], count:2, dtstart:E(2026,1,1)});
        assert(rByWeekday.all().length===2,"byweekday");
        const rByYearDay=new RRule({freq:"YEARLY", byyearday:[1], count:2, dtstart:E(2020,1,1)});
        assert(rByYearDay.all().length===2,"byyearday");
        const rByWeekNo=new RRule({freq:"YEARLY", byweekno:[1], byweekday:["MO"], dtstart:E(2026,1,1), count:2});
        assert(rByWeekNo.all().length===2,"byweekno");
        const rBySetPos=new RRule({freq:"MONTHLY", byweekday:["MO","TU","WE","TH","FR"], bysetpos:[-1], count:2, dtstart:E(2026,1,1)});
        assert(rBySetPos.all().length===2,"bysetpos");
        const rByMonthDay=new RRule({freq:"MONTHLY", bymonthday:[15], count:2, dtstart:E(2026,1,1)});
        assert(rByMonthDay.all().length===2,"bymonthday");
    }
});

__kitS.section("prev daily", () => {
{
        const r=new RRule({freq:"DAILY", dtstart:E(2026,1,1), count:5});
        const p=r.prev(E(2026,1,3));
        assert(p!==null && p.getTime()/1000===E(2026,1,2),"prev daily");
        assertEq(r.next(E(2026,1,2)).getTime()/1000, E(2026,1,3),"next daily");
        assert(r.next(E(2026,1,10))===null,"next past end null");
        const bInc=r.between(E(2026,1,2), E(2026,1,4), true);
        assertEq(bInc.length,3,"between inc true 3");
        const bExc=r.between(E(2026,1,2), E(2026,1,4), false);
        assertEq(bExc.length,1,"between inc false 1");
    }
});

await __kitS.section("toString FREQ", async () => {
{
        const r=new RRule({freq:"MONTHLY", byweekday:["MO"], count:3, dtstart:E(2026,1,5)});
        const s=r.toString();
        assert(s.includes("FREQ=MONTHLY"),"toString FREQ");
        assert(s.includes("BYDAY=MO"),"toString BYDAY");
        const parsed=RRule.fromString(s, {dtstart:E(2026,1,5)});
        assert(JSON.stringify(secs(parsed.all()))===JSON.stringify(secs(r.all())),"fromString roundtrip");
        const str="RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=2";
        const p2=RRule.fromString(str, {dtstart:E(2026,1,5)});
        assertEq(p2.all().length,2,"fromString simple");
        const withDt="DTSTART:20260101T000000Z\nRRULE:FREQ=DAILY;COUNT=2";
        const p3=RRule.fromString(withDt);
        assertEq(p3.all().length,2,"fromString with DTSTART");
        const rrParsed=RRule.fromString("RRULE:FREQ=HOURLY;COUNT=2");
        assert(rrParsed!==null && typeof rrParsed==="object","fromString valid rule parses");
        await assertThrows(()=>new RRule({freq:"SECONDLY", dtstart:E(2020,1,1), count:1e9}).all(2000000),"all beyond cap throws");
    }
});

__kitS.section("RRule large count 10000", () => {
{
        const r=new RRule({freq:"DAILY", dtstart:E(2020,1,1), count:10000});
        const all=r.all();
        assertEq(all.length,10000,"RRule large count 10000");
        const p=r.prev(E(2020,1,20));
        assert(p!==null,"prev not null");
    }
if (fails===0) print("test_time_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_time_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
