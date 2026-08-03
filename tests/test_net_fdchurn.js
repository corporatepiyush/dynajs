/* test_net_fdchurn.js -- descriptor hygiene across the client lifecycle.
 *
 * THE LOW LIMIT IS THE TEST. One leaked descriptor per connect/close cycle is
 * completely invisible at a normal `ulimit -n`: 150 cycles finish and report
 * zero errors, because the run ends long before 4096 descriptors matter. Under
 * a limit of 64 the same leak is fatal inside the loop. Injecting one -- by
 * dropping the `dyn_aio_close` from the Redis dispose -- gives exactly that
 * split, which is what makes this a test rather than a warm-up.
 *
 * Run it through the Makefile target, which sets the limit; run directly it
 * still passes and proves nothing.
 */
import { TCPServer, Redis, PostgreSQL } from "dyna:net";
function bytes(s){const a=new Uint8Array(s.length);for(let i=0;i<s.length;i++)a[i]=s.charCodeAt(i)&0xff;return a;}
function i32(v){return String.fromCharCode((v>>>24)&255,(v>>>16)&255,(v>>>8)&255,v&255);}
function msg(t,b){return t+i32(b.length+4)+b;}
const started=new Map();
const rs=new TCPServer({port:0});
rs.start({data:(c,b)=>{ c.write(bytes("%1\r\n$5\r\nproto\r\n:3\r\n")); }});
const ps=new TCPServer({port:0});
function i16(v){return String.fromCharCode((v>>>8)&255,v&255);}
function cstr(x){return x+"\0";}
ps.start({data:(c,b)=>{
  if(!started.get(c)){ started.set(c,true);
    c.write(bytes(msg("R",i32(0))+msg("K",i32(1)+"abcd")+msg("Z","I"))); return; }
  /* answer any query: one column, one row, then ReadyForQuery */
  const T = msg("T", i16(1)+cstr("one")+i32(0)+i16(0)+i32(23)+i16(0xffff)+i32(0xffffffff)+i16(0));
  const D = msg("D", i16(1)+i32(1)+"1");
  c.write(bytes(T+D+msg("C",cstr("SELECT 1"))+msg("Z","I")));
}});

const N = parseInt(scriptArgs[1]||"150",10);
let done=0, errs=0, i=0;
function step(){
  if(i++>=N){ finish(); return; }
  const R=new Redis({port:rs.port,host:"127.0.0.1"});
  const P=new PostgreSQL({port:ps.port,host:"127.0.0.1",user:"u"});
  Promise.all([R.command("PING").catch(e=>{errs++;}),
               P.query("SELECT 1").catch(e=>{errs++;})])
    .then(()=>{ R.close(); P.close(); done++; step(); });
}
function finish(){
  print("cycles=" + done + " errors=" + errs);
  rs.close(); ps.close();
}
step();
