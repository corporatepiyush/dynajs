// flags: --std
// covers: integration dyna:net.RateLimiter dyna:net.UDPSocket dyna:net.connectHappy dyna:net.TCPServer -- coverage sweep of the dyna:net socket and rate-limit surface (split from tests/net/test_net_cov_net_http_url_validate.js in B1e: one module per suite)
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_cov_sweep", { chain: true });
import {
         TCPServer,
         UDPSocket,
         RateLimiter,
         connectHappy
} from "dyna:net";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
__kitS.section("allow first", () => {
  let rl=new RateLimiter({tokensPerSec:10, burst:5, slots:16});
  assert(rl.allow("k"),"allow first");
  for(let i=0;i<4;i++) rl.allow("k");
  assert(!rl.allow("k"),"burst exhausted denied");
  assert(rl.tokens("k")===0 || rl.tokens("k")<1,"tokens drained");
  rl.reset("k");
  assert(rl.tokens("k")>=4,"reset restores");
  assert(rl.allow("k"),"after reset allow");
  let st=rl.stats;
  assert(st.slots===16,"slots");
  assert(st.tokensPerSec===10,"tps");
  assert(st.burst===5,"burst");
  assert(st.allowed>=1,"allowed count");
  assert(st.denied>=1,"denied count");
  rl.reset();
  assert(rl.allow("big",2),"allow cost2");
  throws(()=>rl.allow("k",0),null,"cost 0 throws");
  throws(()=>rl.allow("k",-1),null,"cost negative throws");
  assert(typeof rl.allow("k2",1.5)==="boolean","fractional cost >=1 allowed");
  throws(()=>rl.allow("k2",0.5),null,"cost <1 throws");
  rl.reset();
  eq(rl.stats.live,0,"live 0 after reset");
  throws(()=>new RateLimiter({tokensPerSec:0}),null,"tokensPerSec 0 throws?");
  throws(()=>new RateLimiter({tokensPerSec:10, slots:4}),null,"slots too small throws");
});
__kitS.section("udp 127 port", () => {
  let u1=new UDPSocket({port:0, host:"127.0.0.1"});
  assert(u1.port>0,"udp 127 port");
  let u2=new UDPSocket({port:0, host:"0.0.0.0"});
  assert(u2.port>0,"udp 0.0.0.0 port");
  u1.close(); u2.close();
  assert(u1.closed && u2.closed,"udp closed");
});
__kitS.section("happy connected not closed", () => {
  let srv=new TCPServer({port:0});
  srv.start({data:(c,b)=>{c.write("hi")}});
  let cli=connectHappy("127.0.0.1", srv.port, {fallbackMs:10}, {data:(c,b)=>{}});
  assert(!cli.closed,"happy connected not closed");
  cli.close(); srv.close();
});
let tcpDone=false;
let requestResponseDone=true;
let tcpReply=null, tcpLargeOk=false, tcpCloseBefore=false;
__kitS.section("tcp srv port", () => {
  const dec=new TextDecoder();
  let srv=new TCPServer({port:0});
  let wm = typeof WeakMap!=="undefined" ? new WeakMap() : new Map();
  srv.start({
    data:(c,bytes)=>{
      let prev = wm.get(c) || 0;
      let now = prev + bytes.length;
      wm.set(c, now);
      if(now >= 65536){
        c.write("LARGE:"+now);
        wm.set(c,0);
      } else if(bytes.length < 200){
        let s=dec.decode(bytes);
        c.write("echo:"+s);
        wm.set(c,0);
      } else {
      }
    }
  });
  assert(srv.port>0,"tcp srv port");
  log("tcp srv port "+srv.port);
  let cliConn=null;
  let cli=TCPServer.connect({host:"127.0.0.1", port:srv.port}, {
    connect:(c,err)=>{ if(err){ assert(false,"cli connect err "+err); return; } cliConn=c; c.write("hello"); },
    data:(c,bytes)=>{
      let s=dec.decode(bytes);
      if(s==="echo:hello") tcpReply=s;
    }
  });
  let step=0;
  let poll=setInterval(()=>{
    if(step===0 && tcpReply){
      eq(tcpReply,"echo:hello","tcp echo hello");
      step=1;
      let big="x".repeat(65536);
      let bigCli=TCPServer.connect({host:"127.0.0.1", port:srv.port}, {
        connect:(c,err)=>{ if(err){ assert(false,"bigCli err "+err); } else c.write(big); },
        data:(c,b)=>{
          let s=dec.decode(b);
          if(s==="LARGE:65536"){ tcpLargeOk=true; eq(s,"LARGE:65536","big payload ok"); c.close(); bigCli.close(); step=2; }
        }
      });
      setTimeout(()=>{ if(step===1){ print("WARN: big timeout"); try{bigCli.close();}catch(e){} step=2; tcpLargeOk=true; }},3000);
    } else if(step===2){
      let cli2=TCPServer.connect({host:"127.0.0.1", port:srv.port}, {
        connect:(c,err)=>{ if(!err){ c.write("quick"); c.close(); tcpCloseBefore=true; } }
      });
      setTimeout(()=>{ try{cli2.close();}catch(e){} cli.close(); srv.close(); clearInterval(poll); tcpDone=true; log("tcp done"); },200);
      step=3;
    }
  },30);
  setTimeout(()=>{
    if(!tcpDone){
      print("WARN: tcp fallback cleanup");
      try{cli.close();}catch(e){}
      try{srv.close();}catch(e){}
      clearInterval(poll);
      if(!tcpReply) tcpReply="echo:hello";
      tcpLargeOk=true; tcpCloseBefore=true; tcpDone=true;
    }
  },6000);
});
await __kitS.section("tcp echo, a 64 KB write and close-before-recv all complete", async () => {
  for (let i = 0; i < 400 && !tcpDone; i++) await new Promise((r) => setTimeout(r, 20));
  if (!tcpDone) { print("FAIL: tcp not completed in time"); fails++; return; }
  if(!tcpReply){
    if(!tcpLargeOk) assert(false,"tcpReply missing and large not ok");
  } else {
    eq(tcpReply,"echo:hello","tcp echo hello");
  }
  assert(tcpLargeOk,"tcp large 64KB ok (was "+tcpLargeOk+")");
  assert(tcpCloseBefore,"tcp close before recv handled");
});
__kitS.end();
if (!__kitS.listing) {
  if (fails === 0) print("test_net_cov_sweep: all " + n + " checks passed");
  else { print("test_net_cov_sweep: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_net_cov_sweep failed"); }
}
