// flags: --std
// covers: integration dyna:dns.DNSResolver dyna:dns.DNSServer -- coverage sweep of the dyna:dns surface (split from tests/net/test_net_cov_net_http_url_validate.js in B1e: one module per suite)
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dns_cov_sweep", { chain: true });
import { DNSResolver, DNSServer } from "dyna:dns";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
__kitS.section("not closed initially", () => {
  let r=new DNSResolver({server:"127.0.0.1", port:5353, timeoutMs:500});
  assert(!r.closed,"not closed initially");
  r.dispose();
  assert(r.closed,"dispose closes");
  throws(()=>r.query("a.com",1),null,"query after dispose throws?");
  let r2=new DNSResolver();
  assert(!r2.closed,"r2 open");
  r2.close();
  assert(r2.closed,"r2 close");
  r2.close();
  assert(r2.closed,"double close");
  let r3=new DNSResolver({server:"127.0.0.1", port:9, timeoutMs:200});
  let done=false;
  let errTag=null;
  r3.query("nowhere.test",1,(err,recs)=>{ errTag=err?String(err):"noerr"; done=true; });
  assert(typeof r3[Symbol.dispose]==="function","Symbol.dispose exists");
  let ds=new DNSServer({port:0, host:"127.0.0.1"});
  assert(!ds.closed,"dns server not closed");
  ds.start((name,type)=> (name==="host.test"&&type===1)?"192.0.2.7":null);
  assert(ds.port>0,"dns server port resolved");
  ds.close();
  assert(ds.closed,"dns server closed");
  ds.close();
  assert(ds.closed,"dns double close");
  setTimeout(()=>{ try{r3.close();}catch(e){} }, 400);
});
await __kitS.section("a resolver aimed at a dead port times out with an error", async () => {
  await new Promise((done) => {
    let settled = false;
    const dr=new DNSResolver({server:"127.0.0.1", port:9, timeoutMs:300});
    dr.query("nowhere2.test",1,(err,recs)=>{
      if (settled) return;
      settled = true;
      assert(err!==null && String(err).length>0,"dead dns timeout error");
      dr.close();
      done();
    });
    setTimeout(()=>{ if(!settled){ settled = true; assert(false,"dns timeout not fired"); try{dr.close();}catch(e){} done(); } },2000);
  });
});
__kitS.end();
if (!__kitS.listing) {
  if (fails === 0) print("test_dns_cov_sweep: all " + n + " checks passed");
  else { print("test_dns_cov_sweep: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_dns_cov_sweep failed"); }
}
