// flags: --std
// Split from tests/test_cov_net_http_url_validate.js: the matcher sections.
import "../lib/httpc.js";
import { Matcher, MultiMatcher, Levenshtein, DiceCoefficient, DiffChars, DiffWords, DiffLines } from "dyna:matcher";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
log("net addresses start");
log("schema done");
log("matcher start");
{
  let m=new Matcher("ss");
  eq(m.firstIn("mississippi"),2,"Matcher firstIn");
  eq(m.firstIn("nope"),-1,"firstIn miss");
  eq(m.countIn("mississippi"),2,"countIn");
  assert(m.test("mississippi"),"test hit");
  assert(!m.test("nope"),"test miss");
  eq(m.length,2,"length");
  eq(m.algo,"kmp","default algo kmp");
  eq(JSON.stringify(new Matcher("aa").allIn("aaaa")),JSON.stringify([0,1,2]),"allIn overlaps");
  eq(JSON.stringify(new Matcher("x").allIn("abc")),JSON.stringify([]),"allIn no match");
  let e=new Matcher("");
  eq(e.firstIn("abc"),0,"empty at 0");
  assert(e.test("abc"),"empty test true");
  eq(e.countIn("abc"),0,"empty count 0");
  eq(JSON.stringify(e.allIn("abc")),JSON.stringify([]),"empty allIn");
  eq(new Matcher("x",{algo:"bmh"}).algo,"bmh","bmh option");
  eq(new Matcher("x",{algo:"boyer-moore"}).algo,"boyer-moore","boyer-moore echoes the accepted spelling (d.ts union)");
  throws(()=>new Matcher("x",{algo:"nope"}),null,"unknown algo throws");
  throws(()=>Matcher("x"),null,"without new throws");
  eq(m.replaceAllIn("mississippi","S"),"miSiSippi","replaceAllIn");
  eq(new Matcher("aa").replaceAllIn("aaaa","-"),"--","non-overlapping replace");
  eq(new Matcher("x").replaceAllIn("abc","-"),"abc","no match unchanged");
  eq(new Matcher("").replaceAllIn("abc","-"),"abc","empty replace nothing");
  {
    let text="café 日本";
    eq(text.length,7,"7 code units");
    eq(new TextEncoder().encode(text).length,12,"12 bytes");
    eq(new Matcher("日").firstIn(text),5,"code units offset 5");
    eq(new Matcher("本").firstIn(text),6,"next char 6");
    let emoji="a😀b";
    eq(emoji.length,4,"emoji 4 code units");
    eq(new Matcher("b").firstIn(emoji),3,"after surrogate 3");
    eq(JSON.stringify(new Matcher("😀").allIn("😀x😀")),JSON.stringify([0,3]),"allIn surrogate");
    eq(new Matcher("é").countIn("ééé"),3,"count multi-byte");
  }
  let patterns=["*","?","**","{a,b}","[!a]","\\*","a*b","a?b","**/a","*.js","foo{bar,baz}","a[!b]c","\\?"];
  for(let pat of patterns){
    let mm=new Matcher(pat);
    assert(mm.test("prefix "+pat+" suffix"),"literal pattern "+JSON.stringify(pat)+" found");
    assert(!mm.test("no match here"),"literal not found for "+pat+" in \"no match\" may accidentally match? but test expects false for unique pattern");
  }
  for(let len of [0,1,2,3,7,15,16,31,32,63,64,127,128]){
    let pat="a".repeat(len);
    let txt="a".repeat(len)+ "X" + "a".repeat(len);
    if(len===0){
      let m0=new Matcher(pat);
      eq(m0.firstIn(txt),0,"N=0 firstIn 0");
    } else {
      let mN=new Matcher(pat);
      eq(mN.firstIn(txt),0,"N="+len+" first at 0");
      eq(mN.countIn(txt),2,"N="+len+" count 2");
      let txt2="b".repeat(len);
      eq(mN.firstIn(txt2),-1,"N="+len+" miss");
    }
  }
  eq(Levenshtein("kitten","sitting"),3,"lev kitten sitting");
  eq(Levenshtein("","",),0,"lev empty");
  eq(Levenshtein("a","a"),0,"lev same");
  eq(Levenshtein("abc","", {max:1}),2,"lev with max returns >max");
  eq(Levenshtein("abc","abc"),0,"lev abc abc");
  assert(DiceCoefficient("hello","hello")===1,"dice same 1");
  assert(DiceCoefficient("hello","hallo")>0 && DiceCoefficient("hello","hallo")<1,"dice partial");
  eq(DiceCoefficient("",""),1,"dice empty 1?");
  {
    let d=DiffChars("abc","abx");
    assert(Array.isArray(d),"DiffChars array");
    assert(d.length>0,"diff not empty");
    let ops=d.map(x=>x.op);
    assert(ops.includes(-1)||ops.includes(1),"diff has change");
  }
  {
    let d=DiffWords("hello world","hello there");
    assert(d.length>0,"DiffWords");
  }
  {
    let d=DiffLines("a\nb\nc","a\nx\nc");
    assert(d.length>0,"DiffLines");
  }
  {
    let mm=new MultiMatcher(["he","she","his","hers"]);
    eq(mm.size,4,"MultiMatcher size");
    assert(mm.states>4,"states >4");
    eq(JSON.stringify(mm.allIn("ushers")),JSON.stringify([{index:1,at:1},{index:0,at:2},{index:3,at:2}]),"aho she/he/hers");
    eq(mm.countIn("ushers"),3,"count 3");
    eq(JSON.stringify(mm.firstIn("ushers")),JSON.stringify({index:1,at:1}),"firstIn");
    assert(mm.test("ushers"),"test hit");
    assert(!mm.test("xyz"),"test miss");
    assert(mm.firstIn("xyz")===null,"firstIn null");
    eq(mm.countIn("xyz"),0,"count miss");
    eq(JSON.stringify(mm.allIn("xyz")),JSON.stringify([]),"allIn miss");
    let router=new MultiMatcher(["GET /api/","POST /api/","DELETE /"]);
    eq(JSON.stringify(router.firstIn("POST /api/users")),JSON.stringify({index:1,at:0}),"router");
    assert(!router.test("PATCH /api/users"),"unrouted");
    let pre=new MultiMatcher(["ab","abc","bc"]);
    eq(JSON.stringify(pre.allIn("abc")),JSON.stringify([{index:0,at:0},{index:1,at:0},{index:2,at:1}]),"prefix/suffix");
    let dup=new MultiMatcher(["aa","aa"]);
    eq(JSON.stringify(dup.allIn("aa")),JSON.stringify([{index:0,at:0}]),"duplicate");
    eq(JSON.stringify(new MultiMatcher(["aa"]).allIn("aaaa")),JSON.stringify([{index:0,at:0},{index:0,at:1},{index:0,at:2}]),"overlaps");
    throws(()=>new MultiMatcher([]),null,"empty list throws");
    throws(()=>new MultiMatcher(["ok",""]),null,"empty pattern throws");
    throws(()=>new MultiMatcher("not array"),null,"non-array throws");
    throws(()=>MultiMatcher(["x"]),null,"without new throws");
    let mm2=new MultiMatcher(["日","本","x"]);
    eq(JSON.stringify(mm2.allIn("café 日本x")),JSON.stringify([{index:0,at:5},{index:1,at:6},{index:2,at:7}]),"MultiMatcher code units");
    let pats=["the","quick","fox","he","ck","e"];
    let text="the quick brown fox jumps over the lazy dog, quickly";
    let single=[];
    pats.forEach((p,i)=>{ for(let at of new Matcher(p).allIn(text)) single.push({index:i,at}); });
    single.sort((a,b)=>a.at-b.at||a.index-b.index);
    let multi=new MultiMatcher(pats).allIn(text).sort((a,b)=>a.at-b.at||a.index-b.index);
    eq(JSON.stringify(multi),JSON.stringify(single),"multi equals N matchers");
    eq(new MultiMatcher(pats).countIn(text), pats.reduce((acc,p)=>acc+new Matcher(p).countIn(text),0),"count same");
  }
  {
    eq("foobar".trimPrefix("foo"),"bar","trimPrefix");
    eq("foobar".trimPrefix("bar"),"foobar","trimPrefix miss");
    eq("foobar".trimSuffix("bar"),"foo","trimSuffix");
    eq("xxhelloyy".trimChars("xy"),"hello","trimChars");
    eq("hello".containsAny("le"),true,"containsAny hit");
    eq("hello".containsAny("xyz"),false,"containsAny miss");
    eq("hello".indexOfAny("le"),1,"indexOfAny");
    eq("hello".indexOfAny("xyz"),-1,"indexOfAny miss");
    eq(JSON.stringify("aaaa".indexOfAll("aa")),JSON.stringify([0,1,2]),"indexOfAll overlaps");
    eq("HeLLo".equalsIgnoreCase("hello"),true,"equalsIgnoreCase");
    eq("a".equalsIgnoreCase("b"),false,"equalsIgnoreCase miss");
    eq("a".compareBytes("b"),-1,"compareBytes less");
    eq("a".compareBytes("a"),0,"equal");
    eq("ab".compareBytes("a"),1,"prefix");
    eq(JSON.stringify("a:b:c".splitN(":",2)),JSON.stringify(["a","b:c"]),"splitN");
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
  if(fails===0) print("test_matcher_cov_net_http_url_validate: all "+n+" checks passed");
  else print("test_matcher_cov_net_http_url_validate: "+fails+" FAILED of "+n+" assertions");
  log("done n="+n+" fails="+fails);
  if(fails) throw new Error("test_matcher_cov_net_http_url_validate failed");
}

dnsChecked = true;
fetchDone = true;
finish();
