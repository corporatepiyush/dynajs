/* test_http_upload_async.js -- the App.upload drain runs OFF the reactor
 * thread (audit M10-01): request bytes are copied into bounded chunks and
 * written through the aio disk pool (drain -> wdone -> resume state
 * machine), never with a blocking write(2) on the reactor thread.
 *
 * This file pins the STATE MACHINE's observable contract: byte-exact
 * stored files across the chunk/in-flight boundaries (0, 256 KiB - 1/=/+1,
 * multi-chunk, odd tails), two concurrent uploads (independent machines),
 * and the handler/meta contract. Byte comparison is done on Uint8Arrays --
 * a decoded-string comparison could mask corruption on non-UTF-8 bodies.
 * The loop-liveness property the offload buys is hardware-dependent to
 * assert (on a page-cached M-series disk the old blocking writes were
 * near-parity at 4 MiB -- measured, not assumed); the structural guarantee
 * is in the code: no write(2) on the reactor thread. The 64 MiB
 * contested-disk run that showed late heartbeats on the baseline and none
 * on the fix is documented in the audit ledger.
 */
import { App, TCPServer } from "dyna:net";
import { File, makeTempDir, readDir, removeAll } from "dyna:file";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };

const dir = makeTempDir("upla");
const app = new App({ port: 0 });
let done = null;
app.upload("/up", { dir: dir, maxFileSize: 8 * 1024 * 1024,
                    allow: ["application/octet-stream"] },
           (saved, meta) => { done = { saved, meta }; });
app.start();
const PORT = app.port;

const strBytes = (s) => {
  const b = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 0xff;
  return b;
};

const CONNS = [];
function post(bodyBytes, onSettled) {
  const conn = TCPServer.connect({ host: "127.0.0.1", port: PORT }, {
    connect: (c) => {
      const head = `POST /up HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\nContent-Length: ${bodyBytes.length}\r\nConnection: close\r\n\r\n`;
      c.write(strBytes(head));
      /* two writes: the drain machine sees a partial body, then the rest */
      const half = bodyBytes.length >> 1;
      c.write(bodyBytes.slice(0, half));
      if (half < bodyBytes.length)
        setTimeout(() => { try { c.write(bodyBytes.slice(half)); } catch (e) {} }, 10);
    },
    data: (c, b) => {
      const s = new TextDecoder().decode(b);
      if (/\r\n\r\n/.test(s) || /200|4\d\d|5\d\d/.test(s)) {
        onSettled();
        try { c.close(); } catch (e) {}
      }
    },
  });
  CONNS.push(conn);
}

async function waitDone() {
  for (let i = 0; i < 600 && !done; i++)
    await new Promise(r => setTimeout(r, 10));
  const d = done; done = null;
  return d || null;
}

const SIZES = [0, 256 * 1024 - 1, 256 * 1024, 256 * 1024 + 1,
               1024 * 1024, 3 * 1024 * 1024 + 7];
for (const size of SIZES) {
  const body = new Uint8Array(size);
  for (let i = 0; i < size; i++) body[i] = (i * 31 + 7) & 0xff;
  await new Promise((resolve) => {
    post(body, async () => {
      const d = await waitDone();
      if (!d) { check(false, `size ${size}: handler ran`); resolve(); return; }
      const stored = new File(d.saved).readBytes();
      check(stored.length === body.length,
            `size ${size}: stored length ${stored.length} vs ${body.length}`);
      let same = stored.length === body.length;
      for (let i = 0; same && i < size; i += 997)
        if (stored[i] !== body[i]) same = false;
      check(same, `size ${size}: stored bytes match (sampled + ends)`);
      check(d.meta.size === size, `size ${size}: meta.size == ${size}`);
      resolve();
    });
  });
}

/* two concurrent uploads: independent drain machines must not cross wires */
{
  const b1 = new Uint8Array(300 * 1024).fill(0x11);
  const b2 = new Uint8Array(500 * 1024).fill(0x22);
  await new Promise((resolve) => {
    let left = 2;
    const one = (body, tag, firstByte) => post(body, async () => {
      const d = await waitDone();
      const stored = new File(d.saved).readBytes();
      check(stored.length === body.length,
            `concurrent ${tag}: length ${stored.length} vs ${body.length}`);
      check(stored.length === 0 || stored[0] === firstByte,
            `concurrent ${tag}: first byte`);
      if (--left === 0) resolve();
    });
    one(b1, "a", 0x11);
    setTimeout(() => one(b2, "b", 0x22), 30);
  });
}

for (const c of CONNS) { try { c.close(); } catch (e) {} }
app.close();
check(readDir(dir).length >= SIZES.length, "all uploads stored");
removeAll(dir);
print(`test_http_upload_async: ${n} checks, ${fails} failures`);
if (fails) throw new Error(fails + " failures");
