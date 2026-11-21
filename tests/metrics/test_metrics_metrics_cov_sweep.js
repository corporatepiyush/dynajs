// flags: --std
// covers: dyna:metrics.Metrics
// coverage sweep of the dyna:metrics surface (split from tests/net/test_net_cov_net_http_url_validate.js in B1e: one module per suite).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_metrics_metrics_cov_sweep", { chain: true });
import { Metrics } from "dyna:metrics";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
__kitS.section("empty after reset", () => {
  Metrics.reset();
  eq(Metrics.scrape(),"","empty after reset");
  Metrics.counter("net_c",2);
  assert(Metrics.scrape().includes("net_c 2"),"counter");
  throws(()=>Metrics.counter("net_c",-1),null,"counter negative throws");
  throws(()=>Metrics.counter("net_c",NaN),null,"counter NaN throws");
  Metrics.gauge("net_g",5);
  assert(Metrics.scrape().includes("net_g 5"),"gauge");
  Metrics.gauge("net_g",-3);
  assert(Metrics.scrape().includes("net_g -3"),"gauge negative ok");
  Metrics.histogram("net_h",0.02);
  let sc=Metrics.scrape();
  assert(sc.includes("net_h_bucket"),"histogram bucket");
  assert(sc.includes("net_h_sum"),"histogram sum");
  throws(()=>Metrics.histogram("net_h",NaN),null,"hist NaN throws");
  throws(()=>Metrics.histogram("net_h",Infinity),null,"hist Inf throws");
  Metrics.histogram("net_h",-0.5);
  assert(Metrics.scrape().includes("net_h"),"hist negative accepted as bucket");
  Metrics.reset();
  eq(Metrics.scrape(),"","reset again empty");
});
__kitS.end();
if (!__kitS.listing) {
  if (fails === 0) print("test_metrics_metrics_cov_sweep: all " + n + " checks passed");
  else { print("test_metrics_metrics_cov_sweep: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_metrics_metrics_cov_sweep failed"); }
}
