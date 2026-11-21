// flags: --std
// timeout: 120
// covers: dyna:postgres.PostgreSQL
import * as std from "std";
import { sections as __kitSections } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";
function bytes(s){const a=new Uint8Array(s.length);for(let i=0;i<s.length;i++)a[i]=s.charCodeAt(i)&0xff;return a;}
function i32(v){return String.fromCharCode((v>>>24)&255,(v>>>16)&255,(v>>>8)&255,v&255);}
function msg(t,b){return t+i32(b.length+4)+b;}
const started=new Map();
const ps=new TCPServer({port:0});
function i16(v){return String.fromCharCode((v>>>8)&255,v&255);}
function cstr(x){return x+"\0";}
ps.start({data:(c,b)=>{
  if(!started.get(c)){ started.set(c,true);
    c.write(bytes(msg("R",i32(0))+msg("K",i32(1)+"abcd")+msg("Z","I"))); return; }
  const T = msg("T", i16(1)+cstr("one")+i32(0)+i16(0)+i32(23)+i16(0xffff)+i32(0xffffffff)+i16(0));
  const D = msg("D", i16(1)+i32(1)+"1");
  c.write(bytes(T+D+msg("C",cstr("SELECT 1"))+msg("Z","I")));
}});

const N = parseInt(scriptArgs[1]||"150",10);
const WATCH_MS = parseInt(scriptArgs[2]||"20000",10);
const WATCH_POLL_MS = Math.max(250, WATCH_MS >> 2);
let done=0, errs=0, i=0, onDone=null;
let progressAt = Date.now();
let phase = "start";
function progress(p){ phase = p; progressAt = Date.now(); }

let watchdog = null;
function armWatchdog() { watchdog = setInterval(() => {
  const idle = Date.now() - progressAt;
  if (idle < WATCH_MS) return;
  clearInterval(watchdog);
  std.err.puts("FAIL: test_postgres_postgre_sql_fdchurn stalled: no completed cycle for " +
               idle + "ms at cycle " + done + "/" + N +
               " (waiting on: " + phase + "); a request or its reply was " +
               "lost between the client and the mock -- failing bounded " +
               "instead of hanging the gate\n");
  std.err.flush();
  std.exit(2);
}, WATCH_POLL_MS); }

function step(){
  if(i++>=N){ finish(); return; }
  progress("cycle " + i + " of " + N + ": pg SELECT");
  const P=new PostgreSQL({port:ps.port,host:"127.0.0.1",user:"u"});
  Promise.all([P.query("SELECT 1").catch(e=>{errs++;})])
    .then(()=>{ P.close(); done++; progress("cycle " + done + " settled"); step(); });
}
function finish(){
  clearInterval(watchdog);
  print("cycles=" + done + " errors=" + errs);
  ps.close();
  if (onDone) onDone();
}
const __kitS = __kitSections("test_postgres_postgre_sql_fdchurn");
await __kitS.section("descriptor churn: every open/use/close cycle settles", () => new Promise((res) => { onDone = res; armWatchdog(); step(); }));
__kitS.end();
