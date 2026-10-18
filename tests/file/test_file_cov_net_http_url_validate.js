// flags: --std
// Split from tests/test_cov_net_http_url_validate.js: the file sections.
import "../lib/httpc.js";
import { Watcher, Path } from "dyna:file";
import * as std from "std";
import * as os from "os";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
log("net addresses start");
{
  let dir=`${std.getenv("TMPDIR") || "/tmp"}/watch_cov_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
  os.exec(["/bin/mkdir","-p",dir],{usePath:true});
  let w=new Watcher(new Path(dir));
  w.start(()=>{});
  let st=w.stats();
  assert(typeof st.truncated==="boolean","truncated boolean");
  assert(typeof st.entries==="number","entries number");
  assert(typeof st.directories==="number","dirs");
  assert(typeof st.events==="number","events");
  w.close(); assert(w.closed,"watcher closed");
  w.close(); assert(w.closed,"double close watcher");
  os.exec(["/bin/rm","-rf",dir],{usePath:true});
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
  if(fails===0) print("test_file_cov_net_http_url_validate: all "+n+" checks passed");
  else print("test_file_cov_net_http_url_validate: "+fails+" FAILED of "+n+" assertions");
  log("done n="+n+" fails="+fails);
  if(fails) throw new Error("test_file_cov_net_http_url_validate failed");
}

dnsChecked = true;
fetchDone = true;
finish();
