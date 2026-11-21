// flags: --std
// timeout: 120
// covers: dyna:redis.Redis
import * as std from "std";
import { sections as __kitSections } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
function bytes(s){const a=new Uint8Array(s.length);for(let i=0;i<s.length;i++)a[i]=s.charCodeAt(i)&0xff;return a;}
const rs=new TCPServer({port:0});
rs.start({data:(c,b)=>{ c.write(bytes("%1\r\n$5\r\nproto\r\n:3\r\n")); }});

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
  std.err.puts("FAIL: test_redis_redis_fdchurn stalled: no completed cycle for " +
               idle + "ms at cycle " + done + "/" + N +
               " (waiting on: " + phase + "); a request or its reply was " +
               "lost between the client and the mock -- failing bounded " +
               "instead of hanging the gate\n");
  std.err.flush();
  std.exit(2);
}, WATCH_POLL_MS); }

function step(){
  if(i++>=N){ finish(); return; }
  progress("cycle " + i + " of " + N + ": redis PING");
  const R=new Redis({port:rs.port,host:"127.0.0.1"});
  Promise.all([R.command("PING").catch(e=>{errs++;})])
    .then(()=>{ R.close(); done++; progress("cycle " + done + " settled"); step(); });
}
function finish(){
  clearInterval(watchdog);
  print("cycles=" + done + " errors=" + errs);
  rs.close();
  if (onDone) onDone();
}
const __kitS = __kitSections("test_redis_redis_fdchurn");
await __kitS.section("descriptor churn: every open/use/close cycle settles", () => new Promise((res) => { onDone = res; armWatchdog(); step(); }));
__kitS.end();
