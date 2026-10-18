// flags: --std
// Split from tests/test_cov_net_http_url_validate.js: the validate sections.
import "../lib/httpc.js";
import { IsAlpha, IsAlphanumeric, IsAscii, IsEmail, IsCreditCard, IsIBAN, IsDomain, IsURL, IsSlug, IsUUID, IsJWT, IsSemver, IsE164 } from "dyna:validate";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
log("net addresses start");
log("url done");
log("validate start");
{
  function ok(v,m){ assert(v===true,m+" true"); }
  function no(v,m){ assert(v===false,m+" false"); }
  ok(IsAlpha("abcXYZ"),"Alpha letters");
  no(IsAlpha("abc1"),"Alpha digit");
  no(IsAlpha(""),"Alpha empty");
  no(IsAlpha("café"),"Alpha accent");
  ok(IsAlphanumeric("abc123"),"Alnum");
  no(IsAlphanumeric("abc-123"),"Alnum dash");
  no(IsAlphanumeric(""),"Alnum empty");
  no(IsAlphanumeric("café"),"Alnum accent");
  ok(IsAscii("hello!~"),"Ascii");
  no(IsAscii("café"),"Ascii latin1");
  no(IsAscii("\u4F60"),"Ascii CJK");
  no(IsAscii(""),"Ascii empty");
  for(let a of ["a@b.co","user@example.com","first.last@example.com","user+tag@example.co.uk"]){
    ok(IsEmail(a),"Email valid "+a);
  }
  for(let a of ["","a","@","a@","@b.co","a..b@c.co","a@b","a@b..co"]){
    no(IsEmail(a),"Email invalid "+JSON.stringify(a));
  }
  ok(IsEmail("a".repeat(64)+"@b.co"),"Email 64 limit");
  no(IsEmail("a".repeat(65)+"@b.co"),"Email 65 over");
  ok(IsIBAN("GB82WEST12345698765432"),"IBAN GB");
  ok(IsIBAN("DE89370400440532013000"),"IBAN DE");
  ok(IsIBAN("GB82 WEST 1234 5698 7654 32"),"IBAN spaces");
  ok(IsIBAN("gb82west12345698765432"),"IBAN lower");
  no(IsIBAN("GB82WEST12345698765433"),"IBAN bad check");
  no(IsIBAN(""),"IBAN empty");
  no(IsIBAN("GB82"),"IBAN short");
  ok(IsCreditCard("4242424242424242"),"card valid");
  ok(IsCreditCard("4242 4242 4242 4242"),"card spaces");
  ok(IsCreditCard("4242-4242-4242-4242"),"card dashes");
  no(IsCreditCard("4242424242424241"),"card bad luhn");
  no(IsCreditCard(""),"card empty");
  no(IsCreditCard("1234"),"card short");
  ok(IsDomain("example.com"),"domain");
  ok(IsDomain("sub.example.co.uk"),"domain multi");
  no(IsDomain("-example.com"),"domain leading hyphen");
  no(IsDomain("example..com"),"domain empty label");
  no(IsDomain("127.0.0.1"),"domain ip literal not domain");
  no(IsDomain("localhost"),"domain no dot");
  ok(IsDomain("a".repeat(63)+".com"),"label 63");
  no(IsDomain("a".repeat(64)+".com"),"label 64");
  ok(IsURL("https://example.com/path?q=1#f"),"IsURL valid");
  ok(IsURL("http://192.168.0.1/x"),"IsURL ip");
  ok(IsURL("mailto:a@b.co"),"IsURL mailto");
  no(IsURL("example.com/path"),"IsURL no scheme");
  no(IsURL("https://"),"IsURL empty host");
  no(IsURL(""),"IsURL empty");
  ok(IsSlug("hello-world"),"slug");
  ok(IsSlug("a"),"slug single");
  no(IsSlug("-hello"),"slug leading hyphen");
  no(IsSlug("hello-"),"slug trailing");
  no(IsSlug("hello--world"),"slug double hyphen");
  no(IsSlug("Hello-world"),"slug uppercase");
  no(IsSlug(""),"slug empty");
  ok(IsSlug("a".repeat(64)),"slug 64 max");
  no(IsSlug("a".repeat(65)),"slug 65 over");
  ok(IsUUID("123e4567-e89b-12d3-a456-426614174000"),"uuid valid");
  no(IsUUID("urn:uuid:123e4567-e89b-12d3-a456-426614174000"),"uuid urn refused");
  no(IsUUID("123e4567e89b12d3a456426614174000"),"uuid raw refused");
  no(IsUUID("123e4567-e89b-12d3-a456-42661417400z"),"uuid non-hex");
  const H="eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
  const C="eyJzdWIiOiIxMjM0NTY3ODkwIn0";
  const S="dGVzdHNpZ25hdHVyZQ";
  ok(IsJWT(H+"."+C+"."+S),"jwt valid");
  no(IsJWT("aaa.bbb"),"jwt 2 segments");
  no(IsJWT("aaa.bbb.ccc.ddd"),"jwt 4 segments");
  no(IsJWT(""),"jwt empty");
  ok(IsSemver("1.2.3"),"semver");
  ok(IsSemver("1.2.3-rc.1+build.2"),"semver prerelease");
  no(IsSemver("1.2"),"semver missing patch");
  ok(IsSemver("v1.2.3"),"semver v prefix allowed (parse contract)");
  no(IsSemver("01.2.3"),"semver leading zero");
  no(IsSemver(""),"semver empty");
  ok(IsE164("+14155552671"),"e164 +");
  ok(IsE164("14155552671"),"e164 without +");
  no(IsE164("1234567890123456"),"e164 16 digits");
  no(IsE164("+1415555267a"),"e164 letter");
  no(IsE164(""),"e164 empty");
  ok(IsE164("123456789012345"),"e164 15 max");
  for(let fn of [IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii, IsDomain, IsURL, IsSlug, IsUUID, IsJWT, IsSemver, IsE164]){
    throws(()=>fn(null),null,"validator non-string throws "+fn.name);
    throws(()=>fn("x".repeat(5000)),null,"validator over-long throws "+fn.name);
  }
  for(let fn of [IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164]){
    no(fn("a".repeat(4096)),"4096 not over cap "+fn.name);
    throws(()=>fn("a".repeat(4097)),null,"4097 throws "+fn.name);
  }
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
  if(fails===0) print("test_validate_cov_net_http_url_validate: all "+n+" checks passed");
  else print("test_validate_cov_net_http_url_validate: "+fails+" FAILED of "+n+" assertions");
  log("done n="+n+" fails="+fails);
  if(fails) throw new Error("test_validate_cov_net_http_url_validate failed");
}

dnsChecked = true;
fetchDone = true;
finish();
