// flags: --std
// covers: integration dyna:netip.Prefix dyna:netip.canonical dyna:netip.compareAddr dyna:netip.contains dyna:netip.isGlobalUnicast dyna:netip.isLinkLocalMulticast dyna:netip.isLinkLocalUnicast dyna:netip.isLoopback dyna:netip.isMulticast dyna:netip.isPrivate dyna:netip.isUnspecified dyna:netip.isValid dyna:netip.masked dyna:netip.parseAddr dyna:netip.parsePrefix -- coverage sweep of the dyna:netip surface (split from tests/net/test_net_cov_net_http_url_validate.js in B1e: one module per suite)
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_netip_cov_sweep", { chain: true });
import { parseAddr, parsePrefix, contains, masked, canonical, isValid, compareAddr, isLoopback, isPrivate, isMulticast, isUnspecified, isLinkLocalUnicast, isGlobalUnicast, isLinkLocalMulticast, Prefix } from "dyna:netip";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
__kitS.section("parseAddr 127 is4", () => {
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
});
__kitS.section("canonical v4", () => {
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
});
__kitS.section("compare less", () => {
  eq(compareAddr("1.1.1.1","2.2.2.2"),-1,"compare less");
  eq(compareAddr("2.2.2.2","1.1.1.1"),1,"greater");
  eq(compareAddr("1.1.1.1","1.1.1.1"),0,"equal");
  eq(compareAddr("255.255.255.255","::1"),-1,"v4 before v6");
  eq(compareAddr("::1","1.1.1.1"),1,"v6 after v4");
  eq(compareAddr("10.0.0.1","10.0.0.2"),-1,"order");
});
__kitS.section("loopback v4", () => {
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
});
__kitS.section("Prefix contains true", () => {
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
});
__kitS.end();
if (!__kitS.listing) {
  if (fails === 0) print("test_netip_cov_sweep: all " + n + " checks passed");
  else { print("test_netip_cov_sweep: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_netip_cov_sweep failed"); }
}
