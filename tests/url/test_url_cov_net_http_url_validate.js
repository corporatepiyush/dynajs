// flags: --std
// Split from tests/test_cov_net_http_url_validate.js: the url sections.
import "../lib/httpc.js";
import { isValid, isLoopback, isPrivate } from "dyna:net";
import { URL, URLSearchParams, domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
log("net addresses start");
log("http done");
log("url start");
{
  let u=new URL("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
  eq(u.protocol,"https:","protocol");
  eq(u.username,"user","username");
  eq(u.password,"pw","password");
  eq(u.hostname,"example.com","hostname");
  eq(u.port,"8443","port");
  eq(u.host,"example.com:8443","host");
  eq(u.pathname,"/a/b","pathname");
  eq(u.search,"?x=1&y=2","search");
  eq(u.hash,"#frag","hash");
  eq(u.origin,"https://example.com:8443","origin");
  eq(u.href,"https://user:pw@example.com:8443/a/b?x=1&y=2#frag","href");
  eq(String(u),u.href,"toString");
  eq(new URL("http://a.com:80/").port,"","dropped 80");
  eq(new URL("https://a.com:443/").port,"","dropped 443");
  eq(new URL("http://a.com:8080/").port,"8080","keep 8080");
  eq(new URL("http://a.com:80/").href,"http://a.com/","href dropped");
  eq(new URL("HTTP://a.com/").protocol,"http:","lowercase scheme");
  eq(new URL("http://[::1]/x").hostname,"[::1]","ipv6 hostname");
  eq(new URL("http://[::1]:8080/x").port,"8080","ipv6 port");
  eq(new URL("http://[2001:db8::1]/").hostname,"[2001:db8::1]","full ipv6");
  eq(new URL("mailto:ada@example.com").protocol,"mailto:","mailto");
  eq(new URL("mailto:ada@example.com").origin,"null","opaque origin null");
  const BASE="http://a/b/c/d;p?q";
  eq(new URL("g",BASE).href,"http://a/b/c/g","relative g");
  eq(new URL("/g",BASE).href,"http://a/g","absolute path");
  eq(new URL("//g",BASE).href,"http://g/","network path (WHATWG: http URLs carry at least /)");
  eq(new URL("../g",BASE).href,"http://a/b/g","dotdot");
  eq(new URL("../../../g",BASE).href,"http://a/g","past root");
  eq(new URL("http://a.com/p").search,"","no query");
  eq(new URL("http://a.com/p").hash,"","no hash");
  eq(new URL("https:\\\\evil.com\\path").href,"https://evil.com/path","backslash normalized");
  eq(new URL("https:\\\\evil.com").host,"evil.com","backslash host");
  eq(new URL("http://a\r\nb/").href,"http://ab/","CRLF stripped from input (WHATWG removes tab/newline before parsing)");
  eq(new URL("http://a\tb/").href,"http://ab/","TAB stripped from input (WHATWG removes tab/newline before parsing)");
  throws(()=>new URL("not a url"),null,"bare throws");
  throws(()=>new URL("/relative/only"),null,"relative no base throws");
  throws(()=>new URL("http://a.com:99999/"),null,"port overflow throws");
  throws(()=>new URL("http://[::1/"),null,"unterminated ipv6 throws");
  for(let ssrf of ["http://127.0.0.1/","http://[::1]/","http://0.0.0.0/","http://10.0.0.1/","http://169.254.0.1/"]){
    let u=new URL(ssrf);
    assert(u.hostname==="127.0.0.1"||u.hostname==="::1"||u.hostname==="0.0.0.0"||u.hostname==="10.0.0.1"||u.hostname==="169.254.0.1"||u.hostname==="[::1]","SSRF hostname parse");
    assert(isPrivate("10.0.0.1")||isLoopback("127.0.0.1"),"ssrf classification");
  }
  throws(()=>new URL("ht!tp://a.com/"),null,"invalid scheme throws");
  let huge="http://a.com/"+ "x".repeat(10000);
  let hu=new URL(huge);
  assert(hu.href.length>10000,"huge url len");
  throws(()=>new URL("http://a.com/"+ "x".repeat(70000)),null,"over-long throws");
  let spU=new URL("http://a.com/path?a=1&b=2&a=3");
  eq(spU.searchParams.get("a"),"1","searchParams get first");
  eq(JSON.stringify(spU.searchParams.getAll("a")),JSON.stringify(["1","3"]),"getAll");
  assert(spU.searchParams.has("b"),"has b");
  spU.searchParams.append("c","4");
  eq(spU.searchParams.get("c"),"4","append");
  spU.searchParams.set("a","9");
  eq(spU.searchParams.get("a"),"9","set");
  spU.searchParams.delete("b");
  assert(!spU.searchParams.has("b"),"delete");
  spU.searchParams.sort();
  assert(spU.searchParams.toString().includes("a=9"),"sort");
  eq(spU.searchParams.size,2,"size");
  try{
    let ours=new URL("http://a/b/c/d;p?q").href;
    let theirs=new globalThis.URL("http://a/b/c/d;p?q").href;
    eq(ours,theirs,"compare with global URL");
  }catch(e){  }
  eq(domainToASCII("münchen.de"),"xn--mnchen-3ya.de","IDNA ascii");
  eq(domainToUnicode("xn--mnchen-3ya.de"),"münchen.de","IDNA unicode");
  eq(punycodeEncode("münchen"),"mnchen-3ya","punycode encode");
  eq(punycodeDecode("mnchen-3ya"),"münchen","punycode decode");
  assert(isValid("127.0.0.1"),"isValid 127");
  eq(encodeURIComponentStrict("!'()~"),"%21%27%28%29%7E","strict escapes !'()~");
  eq(encodeURIComponentStrict("a b"),"a%20b","strict space is %20 (dynajs.d.ts: like encodeURIComponent)");
  throws(()=>encodeURIComponentStrict(42),null,"strict non-string throws");
  eq(formEncode({a:"1",b:"2"}),"a=1&b=2","formEncode");
  eq(formEncode({a:"hello world"}),"a=hello+world","space +");
  eq(formDecode("a=1&b=2").a,"1","formDecode");
  eq(formDecode("a=hello+world").a,"hello world","plus decode");
  eq(formDecode("a=%20").a," ","percent");
  eq(formDecode("a=1&a=2").a,"2","last wins");
  eq(formDecode("__proto__=polluted&x=1").x,"1","proto pollution safe");
  assert(Object.prototype.hasOwnProperty.call(formDecode("__proto__=polluted"),"__proto__"),"own proto");
  {
    let u=new URL("http://a.com/?x=1");
    let sp=u.searchParams;
    sp.set("x","2");
    assert(u.search.includes("x=2")||u.href.includes("x=2"),"bound writes through");
    let standalone=new URLSearchParams("a=1&b=2");
    eq(standalone.get("a"),"1","standalone");
    standalone.append("c","3");
    eq(standalone.get("c"),"3","append standalone");
  }
  let encU=new URL("http://a.com/a b");
  assert(encU.href.includes("%20"),"space encoded in href");
  assert(encU.pathname==="/a%20b","pathname percent-encodes space (WHATWG path set)");
  eq(new URL("http://a.com:0/").port,"0","port 0 kept?");
  throws(()=>new URL("http://a.com:99999/"),null,"port too large");
}
let dnsChecked=false;
let fetchDone=false;
let finished=false;
function finish(){
  if(finished) return;
  if(!dnsChecked || !fetchDone){
    setTimeout(finish,50);
    return;
  }
  finished=true;
  if(fails===0) print("test_url_cov_net_http_url_validate: all "+n+" checks passed");
  else print("test_url_cov_net_http_url_validate: "+fails+" FAILED of "+n+" assertions");
  log("done n="+n+" fails="+fails);
  if(fails) throw new Error("test_url_cov_net_http_url_validate failed");
}

dnsChecked = true;
fetchDone = true;
finish();
