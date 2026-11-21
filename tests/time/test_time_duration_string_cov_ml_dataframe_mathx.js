// covers: dyna:time.durationString
// Split from tests/test_cov_ml_dataframe_mathx.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_duration_string_cov_ml_dataframe_mathx");
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
    

__kitS.section("PlainDate year", () => {
{
        const d=new PlainDate(2026,1,1);
        assertEq(d.year,2026,"PlainDate year");
        assertEq(d.month,1,"PlainDate month");
        assert(d.toString().includes("2026"),"PlainDate toString");
        const dur=new Duration({days:1});
        const d2=d.add(dur);
        assertEq(d2.day,2,"PlainDate add");
        const diff=d.until(d2);
        assertEq(diff.days,1,"PlainDate until");
        const pt=new PlainTime(12,30);
        assertEq(pt.hour,12,"PlainTime hour");
        const fmt=new Format("2006-01-02");
        const sec=date(2026,1,1);
        assert(fmt.format(sec)==="2026-01-01","Format format");
        assertEq(parseDuration("1h"), 3600*1e9,"parseDuration 1h");
        assert(durationString(3600*1e9)==="1h0m0s","durationString 1h");
        assert(durationString(60*1e9)==="1m0s","durationString 1m");
        assert(durationString(1e9)==="1s","durationString 1s");
        assert(durationString(5e8)==="500ms","durationString ms");
        assert(formatRFC3339(sec).includes("2026"),"formatRFC3339");
        const parsed=parseRFC3339("2026-01-01T00:00:00Z");
        assertEq(parsed.sec, sec,"parseRFC3339 roundtrip");
    }
if (fails===0) print("test_time_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_time_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
