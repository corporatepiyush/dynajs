// flags: --std
// covers: dyna:proxy.TCPProxy
// coverage sweep of the dyna:proxy surface (split from tests/net/test_net_cov_net_http_url_validate.js in B1e: one module per suite).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_proxy_tcp_proxy_cov_sweep", { chain: true });
import { TCPProxy } from "dyna:proxy";
import { TCPServer } from "dyna:net";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
__kitS.section("proxy port", () => {
  let srv=new TCPServer({port:0});
  srv.start({data:(c,b)=>{c.write(b)}});
  let p=new TCPProxy({port:0, upstream:{host:"127.0.0.1", port:srv.port}});
  p.start();
  assert(p.port>0,"proxy port");
  let st=p.stats();
  assert(st.live===0,"proxy live 0 initially");
  assert(typeof st.accepted==="number","accepted");
  assert(typeof st.bytesUp==="number","bytesUp");
  p.close();
  assert(p.closed,"proxy closed");
  p.close();
  assert(p.closed,"proxy double close");
  srv.close();
  let srv2=new TCPServer({port:0});
  srv2.start({data:(c,b)=>{}});
  let p2=new TCPProxy({port:0, upstream:[{host:"127.0.0.1", port:srv2.port},{host:"127.0.0.1", port:9}], maxConns:10, idleTimeoutMs:1000, connectTimeoutMs:500});
  p2.start();
  assert(p2.port>0,"proxy2 port");
  p2.close(); srv2.close();
});
__kitS.end();
if (!__kitS.listing) {
  if (fails === 0) print("test_proxy_tcp_proxy_cov_sweep: all " + n + " checks passed");
  else { print("test_proxy_tcp_proxy_cov_sweep: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_proxy_tcp_proxy_cov_sweep failed"); }
}
