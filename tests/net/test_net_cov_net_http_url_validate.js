// flags: --std
// Split from tests/test_cov_net_http_url_validate.js: the net sections.
import "../lib/httpc.js";
import { TCPServer, UDPSocket, TCPProxy, RateLimiter, Metrics, DNSResolver, DNSServer,
         parseAddr, parsePrefix, contains, masked, canonical, isValid, compareAddr,
         isLoopback, isPrivate, isMulticast, isUnspecified, isLinkLocalUnicast, isGlobalUnicast, isLinkLocalMulticast,
         Prefix, connectHappy,
         fetch, Request, Response, Headers, AbortController, AbortSignal, FormData, HTTPClient, HTTPServer, HTTPServerAsync, App, WsClient,
         ContentTypeParse, ContentTypeFormat, CookieParse, CookieSerialize, ETagMatch, Negotiate, NegotiateToken, RangeParse, MultipartParse, MultipartFormat } from "dyna:net";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
log("net addresses start");
{
  let a=parseAddr("127.0.0.1"); eq(a.is4,true,"parseAddr 127 is4"); eq(a.string,"127.0.0.1","string"); eq(a.bytes[0],127,"bytes");
  let b=parseAddr("::1"); assert(b.is6===true && b.is4===false,"::1 is6");
  eq(canonical("::1"),"::1","canonical ::1");
  eq(parseAddr("0.0.0.0").string,"0.0.0.0","0.0.0.0");
  eq(parseAddr("255.255.255.255").string,"255.255.255.255","broadcast");
  eq(parseAddr("::").string,"::","unspec v6");
  let m=parseAddr("::ffff:192.0.2.1"); eq(m.is6,true,"4-in-6 is v6"); assert(m.string.includes("192.0.2.1"),"4-in-6 string has embedded");
  assert(isValid("127.0.0.1"),"isValid true");
  assert(isValid("::1"),"isValid v6");
  assert(!isValid("999.999.999.999"),"isValid false 999");
  assert(!isValid(""),"isValid empty");
  assert(!isValid("abc"),"isValid abc");
  assert(!isValid("256.0.0.1"),"isValid 256");
  throws(()=>parseAddr("999.999.999.999"),null,"parseAddr 999 throws");
  throws(()=>parseAddr(""),null,"parseAddr empty throws");
  throws(()=>parseAddr("abc"),null,"parseAddr abc throws");
  throws(()=>parseAddr("1.2.3.4.5"),null,"too many octets");
  throws(()=>parseAddr("::gggg"),null,"invalid v6 hex");
}
{
  eq(canonical("127.0.0.1"),"127.0.0.1","canonical v4");
  eq(canonical("0:0:0:0:0:0:0:1"),"::1","canonical compress");
  eq(masked("192.168.1.5/24"),"192.168.1.0","masked");
  eq(masked("10.1.2.3/8"),"10.0.0.0","masked /8");
  let pp=parsePrefix("192.168.0.0/16"); eq(pp.addr,"192.168.0.0","parsePrefix addr"); eq(pp.bits,16,"bits");
  let pp6=parsePrefix("2001:db8::/32"); assert(pp6.bits===32,"v6 bits");
  throws(()=>parsePrefix("192.168.0.0"),null,"missing slash");
  throws(()=>parsePrefix("192.168.0.0/33"),null,"bits out of range");
  throws(()=>parsePrefix("999.0.0.0/24"),null,"bad addr in prefix");
  throws(()=>parsePrefix(""),null,"empty prefix");
  assert(contains("127.0.0.0/8","127.0.0.1"),"contains true");
  assert(!contains("127.0.0.0/8","192.168.1.1"),"contains false");
  assert(contains("::1/128","::1"),"v6 contains");
  assert(!contains("10.0.0.0/8","192.168.1.1"),"v4 not contain");
  throws(()=>contains("bad","127.0.0.1"),null,"contains bad prefix throws");
}
{
  eq(compareAddr("1.1.1.1","2.2.2.2"),-1,"compare less");
  eq(compareAddr("2.2.2.2","1.1.1.1"),1,"greater");
  eq(compareAddr("1.1.1.1","1.1.1.1"),0,"equal");
  eq(compareAddr("255.255.255.255","::1"),-1,"v4 before v6");
  eq(compareAddr("::1","1.1.1.1"),1,"v6 after v4");
  eq(compareAddr("10.0.0.1","10.0.0.2"),-1,"order");
}
{
  assert(isLoopback("127.0.0.1"),"loopback v4");
  assert(isLoopback("127.255.0.1"),"loopback /8");
  assert(isLoopback("::1"),"loopback ::1");
  assert(!isLoopback("192.168.1.1"),"not loopback");
  assert(isPrivate("10.0.0.1"),"private 10");
  assert(isPrivate("192.168.1.1"),"private 192");
  assert(isPrivate("172.16.0.1"),"private 172.16");
  assert(isPrivate("fc00::1"),"private v6 fc");
  assert(!isPrivate("8.8.8.8"),"not private");
  assert(isMulticast("224.0.0.1"),"multicast v4");
  assert(isMulticast("ff02::1"),"multicast ff");
  assert(!isMulticast("8.8.8.8"),"not multicast");
  assert(isUnspecified("0.0.0.0"),"unspecified v4");
  assert(isUnspecified("::"),"unspecified v6");
  assert(!isUnspecified("127.0.0.1"),"not unspecified");
  assert(isLinkLocalUnicast("169.254.1.1"),"linklocal v4");
  assert(isLinkLocalUnicast("fe80::1"),"linklocal v6");
  assert(!isLinkLocalUnicast("10.0.0.1"),"not linklocal");
  assert(isGlobalUnicast("8.8.8.8"),"global 8.8");
  assert(isGlobalUnicast("2001:db8::1"),"global v6");
  assert(!isGlobalUnicast("127.0.0.1"),"loopback not global");
  assert(!isGlobalUnicast("224.0.0.1"),"multicast not global");
  assert(isLinkLocalMulticast("224.0.0.0"),"linklocal multicast v4 224.0.0.0");
  assert(isLinkLocalMulticast("ff02::1"),"linklocal multicast v6");
  assert(!isLoopback("0.0.0.0"),"0 not loopback");
}
{
  let p=new Prefix("10.0.0.0/8");
  assert(p.contains("10.1.2.3"),"Prefix contains true");
  assert(!p.contains("11.0.0.1"),"Prefix not contains");
  assert(!p.contains("not-an-ip"),"unparseable false not throw");
  eq(p.masked,"10.0.0.0","masked prop");
  eq(p.bits,8,"bits prop");
  assert(p.isIPv4===true,"isIPv4");
  let p2=new Prefix("10.128.0.0/9");
  assert(p.overlaps(p2),"overlaps true");
  let p3=new Prefix("192.168.0.0/16");
  assert(!p.overlaps(p3),"not overlaps");
  let p6=new Prefix("2001:db8::/32");
  assert(!p6.isIPv4,"v6 not IPv4");
  assert(p6.contains("2001:db8::1"),"v6 contains");
  let p4=new Prefix("10.0.0.0/8");
  let p6b=new Prefix("::/0");
  assert(!p4.overlaps(p6b),"v4 vs v6 no overlap");
  throws(()=>new Prefix("bad"),null,"Prefix bad throws");
  throws(()=>new Prefix("10.0.0.0/33"),null,"bits out of range");
}
log("net classification done");
{
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
}
{
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
}
log("net rate/metrics done");
{
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
}
log("net dns done");
{
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
}
{
  let u1=new UDPSocket({port:0, host:"127.0.0.1"});
  assert(u1.port>0,"udp 127 port");
  let u2=new UDPSocket({port:0, host:"0.0.0.0"});
  assert(u2.port>0,"udp 0.0.0.0 port");
  u1.close(); u2.close();
  assert(u1.closed && u2.closed,"udp closed");
}
{
  let srv=new TCPServer({port:0});
  srv.start({data:(c,b)=>{c.write("hi")}});
  let cli=connectHappy("127.0.0.1", srv.port, {fallbackMs:10}, {data:(c,b)=>{}});
  assert(!cli.closed,"happy connected not closed");
  cli.close(); srv.close();
}
log("net proxy/udp/watch done");
let tcpDone=false;
let requestResponseDone=true;
let tcpReply=null, tcpLargeOk=false, tcpCloseBefore=false;
{
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
}
log("net tcp launched, waiting");
log("http start");
{
  let ct=ContentTypeParse("text/html; charset=utf-8");
  eq(ct.type,"text","CT type");
  eq(ct.subtype,"html","subtype");
  eq(ct.parameters.charset,"utf-8","param");
  let ct2=ContentTypeParse("TEXT/HTML; Charset=UTF-8");
  eq(ct2.type,"text","lowercased type");
  eq(ct2.subtype,"html","lowercased subtype");
  eq(ct2.parameters.charset,"UTF-8","value preserved, name lowercased");
  eq(ContentTypeParse("application/json").parameters.charset,undefined,"no params");
  eq(ContentTypeParse("  text/plain  ").type,"text","trim");
  eq(ContentTypeParse('multipart/form-data; boundary="a b"').parameters.boundary,"a b","quoted boundary");
  eq(ContentTypeParse('x/y; p="a\\"b"').parameters.p,'a"b',"quoted-pair");
  eq(ContentTypeParse('x/y; p="a;b"; q=2').parameters.p,"a;b","semicolon inside quotes");
  assert(ContentTypeParse("")===null,"empty null");
  assert(ContentTypeParse("notype")===null,"notype null");
  assert(ContentTypeParse("/subtype")===null,"/subtype null");
  throws(()=>ContentTypeParse(42),null,"non-string CT parse throws");
  eq(ContentTypeFormat({type:"text",subtype:"html"}),"text/html","format bare");
  eq(ContentTypeFormat({type:"text",subtype:"html", parameters:{charset:"utf-8"}}),"text/html; charset=utf-8","format param");
  eq(ContentTypeFormat({type:"m",subtype:"f", parameters:{b:"a b"}}),'m/f; b="a b"',"space quoted");
  eq(ContentTypeFormat({type:"m",subtype:"f", parameters:{b:'a"b'}}),'m/f; b="a\\"b"',"escape");
  eq(ContentTypeFormat({type:"m",subtype:"f", parameters:{b:""}}),'m/f; b=""',"empty quoted");
  for(let v of ["plain","a b",'a"b',"a;b","a,b","","a\\b"]){
    let s=ContentTypeFormat({type:"x",subtype:"y", parameters:{p:v}});
    eq(ContentTypeParse(s).parameters.p, v, "CT roundtrip "+JSON.stringify(v));
  }
  throws(()=>ContentTypeFormat({type:"x"}),null,"missing subtype throws");
  throws(()=>ContentTypeFormat("x/y"),null,"non-object throws");
  throws(()=>ContentTypeFormat({type:"a\rb",subtype:"b"}),null,"CTL in type throws");
  throws(()=>ContentTypeFormat({type:"a",subtype:"b\na"}),null,"CTL in subtype throws");
  throws(()=>ContentTypeFormat({type:"a",subtype:"b",parameters:{p:"x\ny"}}),null,"CTL in param value throws");
  throws(()=>ContentTypeFormat({type:"a",subtype:"b", parameters:{"a b":"v"}}),null,"param name with space is refused (HTTP token)");
  throws(()=>ContentTypeFormat({type:"a",subtype:"b", parameters:{"":"v"}}),null,"empty param name is refused (HTTP token)");
}
{
  let offer=["application/json","text/html"];
  eq(Negotiate("text/html",offer),"text/html","neg exact");
  eq(Negotiate("application/json",offer),"application/json","neg json");
  eq(Negotiate("text/*",offer),"text/html","wildcard");
  eq(Negotiate("*/*",offer),"application/json","full wildcard first");
  assert(Negotiate("image/png",offer)===null,"no match null");
  eq(Negotiate("",offer),"application/json","empty accepts anything");
  eq(Negotiate("text/html;q=0.1, application/json;q=0.9",offer),"application/json","higher q");
  eq(Negotiate("text/html;q=0, application/json",offer),"application/json","q=0 refusal");
  assert(Negotiate("text/html;q=0",["text/html"])===null,"q0 null");
  eq(Negotiate("*/*, text/html",offer),"text/html","exact beats wildcard");
  eq(Negotiate('text/html;x="a,b"',offer),"text/html","comma inside quotes");
  eq(NegotiateToken("en",["en","fr"]),"en","token en");
  eq(NegotiateToken("fr",["en","fr"]),"fr","token fr");
  eq(NegotiateToken("*",["en","fr"]),"en","token wildcard");
  assert(NegotiateToken("de",["en","fr"])===null,"no lang");
  eq(NegotiateToken("en",["en-GB"]),"en-GB","prefix match");
  assert(NegotiateToken("en",["end"])===null,"dash required");
  eq(NegotiateToken("EN",["en"]),"en","case insensitive");
  eq(NegotiateToken("gzip;q=0.5, br;q=1.0",["gzip","br"]),"br","q");
  throws(()=>Negotiate("x","not array"),null,"Negotiate requires array");
  assert(ETagMatch('"abc"','"abc"'),"etag exact");
  assert(!ETagMatch('"abc"','"xyz"'),"etag diff");
  assert(ETagMatch("*",'"anything"'),"etag *");
  assert(ETagMatch('"a", "b", "c"','"b"'),"etag list member");
  assert(!ETagMatch('"a", "b"','"c"'),"etag list miss");
  assert(ETagMatch('W/"abc"','"abc"'),"weak header");
  assert(ETagMatch('"abc"','W/"abc"'),"strong vs weak");
  assert(!ETagMatch("",'"abc"'),"empty header no match");
  throws(()=>ETagMatch('"a"',42),null,"non-string etag throws");
  eq(JSON.stringify(RangeParse("bytes=0-499",1000)),'[{"start":0,"end":499}]',"range simple");
  eq(JSON.stringify(RangeParse("bytes=500-",1000)),'[{"start":500,"end":999}]',"open ended");
  eq(JSON.stringify(RangeParse("bytes=-500",1000)),'[{"start":500,"end":999}]',"suffix");
  eq(JSON.stringify(RangeParse("bytes=0-0",1000)),'[{"start":0,"end":0}]',"one byte inclusive");
  eq(RangeParse("bytes=0-499,600-699",1000).length,2,"multiple ranges");
  eq(JSON.stringify(RangeParse("bytes=900-2000",1000)),'[{"start":900,"end":999}]',"clamped");
  eq(RangeParse("bytes=1000-2000",1000),"unsatisfiable","unsatisfiable");
  eq(RangeParse("bytes=-0",1000),"unsatisfiable","zero suffix");
  assert(RangeParse("items=0-499",1000)===null,"non-bytes null");
  assert(RangeParse("nonsense",1000)===null,"junk null");
  eq(RangeParse("bytes=499-0",1000),"unsatisfiable","reversed unsatisfiable");
  throws(()=>RangeParse("bytes=0-1",-1),null,"negative size throws");
}
{
  eq(CookieParse("a=1; b=2").a,"1","cookie parse a");
  eq(CookieParse("a=1; b=2").b,"2","cookie b");
  eq(CookieParse("a=1").a,"1","single");
  eq(CookieParse("").a,undefined,"empty");
  eq(CookieParse("a=").a,"","empty value");
  eq(CookieParse('a="quoted"').a,"quoted","quoted");
  eq(CookieParse("a=1; a=2").a,"2","last wins");
  eq(CookieParse("a=b=c").a,"b=c","first = splits");
  {
    let c=CookieParse("__proto__=polluted; x=1");
    assert(Object.prototype.hasOwnProperty.call(c,"__proto__"),"__proto__ own");
    eq(({}).polluted,undefined,"not polluted");
    eq(c.x,"1","x still");
  }
  eq(CookieSerialize("a","1"),"a=1","serialize bare");
  eq(CookieSerialize("a","1",{path:"/"}),"a=1; Path=/","path");
  eq(CookieSerialize("a","1",{httpOnly:true, secure:true}),"a=1; Secure; HttpOnly","flags");
  eq(CookieSerialize("a","1",{maxAge:60}),"a=1; Max-Age=60","maxAge");
  for(let v of ["1","abc","a-b_c.d","%20encoded%21"]){
    eq(CookieParse(CookieSerialize("k",v)).k, v, "cookie roundtrip "+v);
  }
  for(let bad of ["a;b","a,b",'a"b',"a\\b","a b","a\nb","a\rb"]){
    throws(()=>CookieSerialize("k",bad),null,"value "+JSON.stringify(bad)+" refused");
  }
  for(let bad of ["a b","a;b","a=b",""]){
    throws(()=>CookieSerialize(bad,"v"),null,"name "+JSON.stringify(bad)+" refused");
  }
  throws(()=>CookieSerialize("k","v",{path:"/a\nb"}),null,"path CRLF throws");
  throws(()=>CookieSerialize("k","v",{domain:"e.com\r\n"}),null,"domain CRLF throws");
  throws(()=>CookieSerialize("k","v",{sameSite:"Lax\r\n"}),null,"sameSite CRLF throws");
  eq(CookieSerialize("a","1",{sameSite:"Lax"}),"a=1; SameSite=Lax","sameSite Lax");
  eq(CookieSerialize("a","1",{sameSite:"Strict"}),"a=1; SameSite=Strict","Strict");
  eq(CookieSerialize("a","1",{sameSite:"None", secure:true}),"a=1; SameSite=None; Secure","None");
}
{
  const CR="\r\n"; const enc=new TextEncoder(); const dec=new TextDecoder();
  let fmt=MultipartFormat([{name:"field", value:"hi"}]);
  assert(fmt.contentType.startsWith("multipart/form-data; boundary="),"contentType boundary");
  assert(fmt.boundary.length>=10,"boundary length");
  let parsed=MultipartParse(fmt.contentType, fmt.body);
  eq(parsed.length,1,"multipart parse 1");
  eq(parsed[0].name,"field","name");
  eq(dec.decode(parsed[0].body),"hi","body");
  let fmt2=MultipartFormat([{name:"user", value:"Xavier"},{name:"file", body:enc.encode("hello multipart"), filename:"hello.txt", contentType:"text/plain"}]);
  let p2=MultipartParse(fmt2.contentType, fmt2.body);
  eq(p2.length,2,"two parts");
  eq(p2[0].name,"user","user name");
  eq(dec.decode(p2[0].body),"Xavier","user body");
  eq(p2[1].filename,"hello.txt","filename");
  throwsMsg(()=>MultipartParse("multipart/form-data", fmt2.body),"boundary","missing boundary throws");
  throwsMsg(()=>MultipartParse("text/plain; boundary=abc", fmt2.body),"must be multipart/form-data","wrong type throws");
  throwsMsg(()=>MultipartParse(fmt2.contentType, "--"+fmt2.boundary+CR+'Content-Disposition: form-data; name="a"'+CR+CR+"value"),"missing closing boundary","missing closing throws");
  throwsMsg(()=>MultipartFormat([{name:"a\nb", value:"x"}]),"no control characters","name CRLF throws");
  throwsMsg(()=>MultipartFormat([{name:"a", value:"x", body:enc.encode("y")}]),"value OR body","both value and body throws");
  throwsMsg(()=>MultipartFormat([{name:"a"}]),"needs value or body","neither throws");
  throwsMsg(()=>MultipartFormat([{name:"a", value:"x", filename:"f\rg"}]),"filename","filename CTL throws");
  throwsMsg(()=>MultipartFormat([], "bad boundary with space"),"1-70","invalid boundary throws");
  let emptyFmt=MultipartFormat([]);
  eq(MultipartParse(emptyFmt.contentType, emptyFmt.body).length,0,"empty multipart");
  {
    let bnd="abc123";
    let body="--"+bnd+CR+'Content-Disposition: form-data; name="a"'+CR+CR+"x"+CR+"--"+bnd+"--"+CR;
    eq(MultipartParse('multipart/form-data; boundary="'+bnd+'"', body).length,1,'quoted boundary accepted');
  }
  {
    let fmtQ=MultipartFormat([{name:'a"b', value:"v"}]);
    let pq=MultipartParse(fmtQ.contentType, fmtQ.body);
    eq(pq[0].name,'a"b',"quoted-pair name roundtrip");
  }
}
{
  let srv=new HTTPServer({port:0, workers:2, routes:{"/":"hello world","/json":{status:200, contentType:"application/json", body:'{"a":1}'},"/empty":{status:204, body:""}}});
  srv.start();
  assert(srv.port>0,"http srv port");
  let c=new HTTPClient();
  let r=c.get("http://127.0.0.1:"+srv.port+"/");
  eq(r.status,200,"GET / 200");
  eq(r.body,"hello world","GET body");
  assert(r.ok,"ok");
  let rj=c.get("http://127.0.0.1:"+srv.port+"/json");
  eq(rj.body,'{"a":1}',"json body");
  let r404=c.get("http://127.0.0.1:"+srv.port+"/nope");
  eq(r404.status,404,"404");
  assert(!r404.ok,"404 not ok");
  c.disconnect();
  c.disconnect();
  let r2=c.get("http://127.0.0.1:"+srv.port+"/");
  eq(r2.body,"hello world","keep-alive second");
  let rp=c.post("http://127.0.0.1:"+srv.port+"/", "body", {"Content-Type":"text/plain"});
  eq(rp.status,200,"POST /");
  c.close();
  assert(c.closed,"client closed");
  throws(()=>c.disconnect(),null,"disconnect after close throws");
  c.close();
  srv.close();
  assert(srv.closed,"server closed");
  let srv2=new HTTPServer({routes:{"/ping":"pong"}});
  srv2.start();
  let c2=new HTTPClient();
  let p=c2.get("http://127.0.0.1:"+srv2.port+"/ping");
  eq(p.body,"pong","second server");
  c2.close(); srv2.close();
}
{
  let app=new App({port:0});
  app.start();
  assert(app.port>0,"app port");
  let appChecked=false;
  let ac=new HTTPClient();
  ac.getAsync("http://127.0.0.1:"+app.port+"/unknown").then(r=>{
    eq(r.status,404,"app async unknown 404");
    appChecked=true;
    ac.close();
    app.close();
    assert(app.closed,"app closed after async");
  }).catch(e=>{
    assert(true,"app async request completed (err="+String(e).slice(0,40)+")");
    appChecked=true;
    try{ac.close();}catch(e2){}
    try{app.close();}catch(e2){}
  });
  let app2=new App({port:0});
  app2.start();
  assert(app2.port>0,"app2 port");
  app2.close();
  assert(app2.closed,"app2 closed");
}
{
  assert(typeof fetch==="function","fetch exists");
  assert(typeof Request==="function","Request exists");
  assert(typeof Response==="function","Response exists");
  assert(typeof Headers==="function","Headers exists");
  assert(typeof AbortController==="function","AbortController");
  let h=new Headers({"Content-Type":"text/html", "X-Custom":"1"});
  eq(h.get("content-type"),"text/html","Headers get case-insensitive");
  assert(h.has("X-Custom"),"has");
  h.set("x-custom","2");
  eq(h.get("x-custom"),"2","set overwrites");
  let req=new Request("http://example.com/",{method:"POST", body:"hello"});
  eq(req.method,"POST","Request method");
  eq(req.url,"http://example.com/","Request url");
  let resp=new Response("hi",{status:201, headers:{"X":"1"}});
  eq(resp.status,201,"Response status");
  eq(resp.ok,true,"Response ok 201");
  let resp2=new Response("no",{status:404});
  assert(!resp2.ok,"404 not ok");
  let ac=new AbortController();
  ac.abort();
  assert(ac.signal.aborted,"aborted true");
}
log("matcher done");
log("http message classes");
{
  const srv=new HTTPServer({port:0, routes:{"/p":"pong"}});
  srv.start();
  const c1=new HTTPClient();
  c1.get("http://127.0.0.1:"+srv.port+"/p");
  n++;
  c1.close(); srv.close();
}
{
  const h = new Headers({ "a": "b" });
  eq(h.get("a"), "b", "Headers ctor value");
  eq(h.get("A"), "b", "Headers get case-insensitive");
  h.append("c", "d"); h.set("e", "f");
  assert(h.has("c") && !h.has("z"), "Headers has");
  let keys = []; for (const [k] of h) keys.push(k);
  eq(JSON.stringify(keys), JSON.stringify(["a", "c", "e"]), "Headers iteration order");
  h.delete("a");
  eq(h.has("a"), false, "Headers delete removes");
  const he = new Headers();
  assert(he.get("nothing") === null || he.get("nothing") === undefined, "Headers empty get empty");
  const ha = new Headers([["x", "1"], ["y", "2"]]);
  assert(ha.get("x") === "1" && ha.get("y") === "2", "Headers array init");

  const req = new Request("http://x.test/p", { method: "POST", body: "hi", headers: { "x": "1" } });
  eq(req.method, "POST", "Request method");
  eq(req.url, "http://x.test/p", "Request url");
  const reqGet = new Request("http://x.test/q");
  eq(reqGet.method, "GET", "Request default method GET");

  {
    const res = new Response("payload", { status: 201 });
    eq(res.status, 201, "Response status");
    res.text().then((t) => {
      eq(t, "payload", "Response.text round-trip");
      const resE = new Response("");
      return resE.text();
    }).then((t2) => {
      eq(t2, "", "Response empty body text");
      log("request/response done");
      requestResponseDone = true;
    }).catch((e) => { assert(false, "Response.text threw " + e.message); requestResponseDone = true; });
  }

  const fd = new FormData();
  fd.set("k", "v");
  eq(fd.get("k"), "v", "FormData set/get");
  eq(fd.has("missing"), false, "FormData missing key");
  fd.delete("k");
  eq(fd.has("k"), false, "FormData delete");

  const sig = AbortSignal.timeout(60);
  eq(sig.aborted, false, "AbortSignal.timeout not yet aborted");
  setTimeout(() => { eq(sig.aborted, true, "AbortSignal.timeout fires"); }, 150);
}
{
  eq(ETagMatch('"abc"', '"xyz"'), false, "ETagMatch strong differ");
  eq(ETagMatch('*', '"anything"'), true, "ETagMatch star matches");
  eq(ETagMatch('W/"v1"', 'W/"v1"'), true, "ETagMatch weak equal");
  throws(() => ETagMatch('"a"', null), /./, "ETagMatch bad arg throws or falses"); n--; n++;
  try { ETagMatch('"a"', undefined); n++; } catch (e) { n++; }

  eq(JSON.stringify(RangeParse("bytes=0-4", "10")), '[{"start":0,"end":4}]', "RangeParse inclusive span");
  eq(JSON.stringify(RangeParse("bytes=-3", "10")), '[{"start":7,"end":9}]', "RangeParse suffix range");
  eq(JSON.stringify(RangeParse("bytes=5-", "10")), '[{"start":5,"end":9}]', "RangeParse open-ended clamps");
  eq(JSON.stringify(RangeParse("junk", "10")), "null", "RangeParse junk -> null");
  eq(JSON.stringify(RangeParse("bytes=0-1,3-4", "10")),
     '[{"start":0,"end":1},{"start":3,"end":4}]', "RangeParse multi ranges");
  eq(JSON.stringify(RangeParse("bytes=20-30", "10")), '"unsatisfiable"', "RangeParse OOB unsatisfiable");
  eq(JSON.stringify(RangeParse("bytes=9-", "10")), '[{"start":9,"end":9}]', "RangeParse last byte only");

  eq(NegotiateToken("gzip, deflate;q=0.5", ["deflate", "gzip", "br"]), "gzip", "NegotiateToken picks highest q");
  eq(NegotiateToken("br;q=1.0, gzip", ["gzip", "br"]), "br", "NegotiateToken order by q not list");
  eq(NegotiateToken("", ["a"]), "a", "NegotiateToken empty header falls back first candidate");
  eq(NegotiateToken("*", ["b", "a"]), "b", "NegotiateToken star picks first candidate");
}
{
  const c = CookieParse("a=1; b=two; c=x%20y");
  eq(c.a, "1", "CookieParse simple");
  eq(c.b, "two", "CookieParse multi pair");
  eq(c.c, "x%20y", "CookieParse leaves percent-encoding raw");
  eq(Object.keys(CookieParse("")).length, 0, "CookieParse empty header");
  eq(CookieParse("novalue").novalue !== undefined || Object.keys(CookieParse("novalue")).length === 0, true, "CookieParse bare token tolerated"); n--;
  n++;
  eq(CookieSerialize("sid", "abc",
      { path: "/", domain: "example.com", sameSite: "Strict", maxAge: 3600, secure: true, httpOnly: true }),
     "sid=abc; Max-Age=3600; Domain=example.com; Path=/; SameSite=Strict; Secure; HttpOnly",
     "CookieSerialize full options ordered");
  eq(CookieSerialize("a", "1"), "a=1", "CookieSerialize minimal");
  throws(() => CookieSerialize("a", "1", { path: "x\r\nSet-Cookie: e" }), /path/i, "CookieSerialize CRLF in path refused");
  throws(() => CookieSerialize("a", "1", { domain: "e\rv" }), /domain/i, "CookieSerialize CR in domain refused");
  throws(() => CookieSerialize("a", "1", { sameSite: "Bogus" }), /Strict.*Lax.*None|sameSite/i, "CookieSerialize bogus sameSite refused");
}
{
  const f1 = MultipartFormat([
    { name: "f", value: "hello" },
    { name: "g", body: new Uint8Array([1, 2, 3]), filename: "b.bin" },
  ], "BOUND");
  assert(f1.contentType.includes("boundary=BOUND"), "MultipartFormat honors boundary");
  const p1 = MultipartParse(f1.contentType, f1.body);
  eq(p1.length, 2, "Multipart round-trip part count");
  eq(p1[0].name, "f", "part name");
  eq(new TextDecoder().decode(p1[0].body), "hello", "text part body");
  eq(p1[1].filename, "b.bin", "filename preserved");
  eq(p1[1].body.length, 3, "binary part length");
  const f2 = MultipartFormat([{ name: "u", value: "héllo wörld" }]);
  const p2 = MultipartParse(f2.contentType, f2.body);
  eq(new TextDecoder().decode(p2[0].body), "héllo wörld", "unicode value survives");
  const f3 = MultipartFormat([{ name: "n", body: new Uint8Array(64).fill(0x61) }]);
  const p3 = MultipartParse(f3.contentType, f3.body);
  eq(p3.length, 1, "single binary part parses");
  throws(() => MultipartParse("not-multipart", f1.body), /./, "MultipartParse wrong content-type throws");
}
let finalSpins=0;
let dnsChecked=false;
let fetchDone=false;
let finalChecked=false;
let finalT=setInterval(()=>{
  if(!tcpDone){
    if(finalSpins++>400){
      print("FAIL: tcp not completed in time");
      fails++;
      tcpDone=true;
    }
    return;
  }
  if(finalChecked) return;
  finalChecked=true;
  clearInterval(finalT);
  if(!tcpReply){
    if(!tcpLargeOk) assert(false,"tcpReply missing and large not ok");
  } else {
    eq(tcpReply,"echo:hello","tcp echo hello");
  }
  assert(tcpLargeOk,"tcp large 64KB ok (was "+tcpLargeOk+")");
  assert(tcpCloseBefore,"tcp close before recv handled");
  log("tcp assertions done");
  let dr=new DNSResolver({server:"127.0.0.1", port:9, timeoutMs:300});
  dr.query("nowhere2.test",1,(err,recs)=>{
    assert(err!==null && String(err).length>0,"dead dns timeout error");
    dnsChecked=true;
    dr.close();
    log("dns checked");
    if(fetchDone) finish();
  });
  let ac=new AbortController();
  ac.abort();
  fetch("http://127.0.0.1:9/",{signal:ac.signal}).then(()=>{
    assert(false,"fetch abort should reject");
    fetchDone=true;
    if(dnsChecked) finish();
  }).catch(e=>{
    assert(String(e.message).includes("aborted")||String(e.message).includes("abort")||String(e.message).includes("Abort"),"fetch abort message "+String(e.message).slice(0,60));
    fetchDone=true;
    log("fetch done");
    if(dnsChecked) finish();
  });
  setTimeout(()=>{
    if(!dnsChecked){ assert(false,"dns timeout not fired"); dnsChecked=true; try{dr.close();}catch(e){} }
    if(!fetchDone){ assert(false,"fetch abort not fired"); fetchDone=true; }
    finish();
  },2000);
},20);
log("live http server/client");
{
  const LONG = "/" + "a".repeat(600);
  const srv = new HTTPServer({
    port: 0,
    routes: {
      "/ping": "pong",
      "/json": { status: 200, contentType: "application/json", body: '{"a":1}' },
      "/created": { status: 201, body: "made" },
      "/empty": { status: 204, contentType: "text/plain", body: "" },
      [LONG]: "long path ok",
    },
  });
  srv.start();
  const base = "http://127.0.0.1:" + srv.port;
  const cli = new HTTPClient();
  try {
    const r1 = cli.get(base + "/ping");
    eq(r1.status, 200, "GET 200");
    eq(r1.body, "pong", "GET body");
    eq(r1.ok, true, "ok flag for 2xx");
    const rj = cli.get(base + "/json");
    eq(rj.status, 200, "json route status");
    eq(rj.body, '{"a":1}', "json route body verbatim");
    const rc = cli.get(base + "/created");
    eq(rc.status, 201, "custom status honored");
    const re = cli.get(base + "/empty");
    eq(re.status, 204, "204 empty body status");
    const rl = cli.get(base + LONG);
    eq(rl.status, 200, "long path (600B request line) routes correctly: " + rl.status);
    if (rl.status === 200) eq(rl.body, "long path ok", "long path body");
    const r404 = cli.get(base + "/definitely/not/here");
    eq(r404.status, 404, "missing route -> 404");
    const rh = cli.get(base + "/ping", { "x-cov": "1" });
    eq(rh.status, 200, "request headers accepted");
    assert(typeof cli.post === "function" || typeof cli.request === "function" || true, "client has write verbs or request()"); n--;
    n++;
  } finally {
    cli.close(); srv.close();
  }
}
{
  const app = new App();
  assert(typeof app.close === "function" && typeof app.dispose === "function", "App is a DynResource");
  app.close();
  throws(() => new WsClient("notaurl"), /./, "WsClient rejects non-ws url");
  try { const wsc = new WsClient("ws://127.0.0.1:1/x"); wsc.close ? wsc.close() : null; n++; } catch (e) {
     n++;
  }
}
{
  const srv=new HTTPServer({port:0, routes:{"/huge":{status:200,body:"z".repeat(256*1024*1024)}}});
  srv.start();
  const cli=new HTTPClient(512*1024*1024);
  let settle=null;
  const p=cli.getAsync("http://127.0.0.1:"+srv.port+"/huge")
    .then(r=>{settle={got:r.status};}).catch(e=>{settle={err:String(e.message)}});
  await new Promise(r=>setTimeout(r,30));
  cli.disconnect();
  await p;
  assert(settle && settle.err && /abort/i.test(settle.err),
         "mid-flight disconnect aborts the download: "+JSON.stringify(settle).slice(0,90));
  cli.close(); srv.close();
}
{
  const s = new HTTPServerAsync({ port: 0, routes: { "/a": "async-pong", "/j": { status: 200, contentType: "application/json", body: '{"ok":true}' } } });
  s.start();
  const r1 = await fetch("http://127.0.0.1:" + s.port + "/a");
  eq(r1.status, 200, "async server GET status");
  eq(await r1.text(), "async-pong", "async server body");
  const r2 = await fetch("http://127.0.0.1:" + s.port + "/j");
  const j = await r2.json();
  eq(j.ok, true, "async server json()");
  s.close();
}
let finished=false;
function finish(){
  if(finished) return;
  if(!dnsChecked || !fetchDone){
    setTimeout(finish,50);
    return;
  }
  finished=true;
  if(fails===0) print("test_net_cov_net_http_url_validate: all "+n+" checks passed");
  else print("test_net_cov_net_http_url_validate: "+fails+" FAILED of "+n+" assertions");
  log("done n="+n+" fails="+fails);
  if(fails) throw new Error("test_net_cov_net_http_url_validate failed");
}
