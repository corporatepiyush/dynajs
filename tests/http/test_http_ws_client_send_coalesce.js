// flags: --std
// timeout: 60
// covers: dyna:http.WsClient
// WsClient.send() inside an event-loop dispatch (a message or open handler) is held (DYN_AIO_HOLD)
// and leaves with the other frames of the same dispatch in one socket send, as the App's server side
// already did; it used to be one send per frame. Measured with getrusage's msgsnd (socket messages
// sent), which macOS counts; where the kernel leaves it 0 (Linux) only the data checks run. An echo of
// N 64-byte binary messages, 32 in flight, client and App server in this process: about 1.0 sends per
// message before (the client's one per frame plus the server's batched ones), about 0.1 after; the
// bound is 0.5N. A send from a timer runs outside any dispatch, where nothing is held: those frames
// must still go out and be echoed, and close() after a held send still delivers the frame first.
// Every echoed byte is checked.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_ws_client_send_coalesce");
import * as std from "std";
import * as os from "os";
import { App, WsClient } from "dyna:http";
import { rusage } from "dyna:sys";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

const SIZE = 64, N = 2000, DEPTH = 32;
const frame = (k) => { const u = new Uint8Array(SIZE); for (let i = 0; i < SIZE; i++) u[i] = (k * 31 + i) & 255; return u; };
const same = (d, k) => { const u = new Uint8Array(d); if (u.length !== SIZE) return false; for (let i = 0; i < SIZE; i++) if (u[i] !== ((k * 31 + i) & 255)) return false; return true; };

let lastSrv = null;
const app = new App({ port: 0, idleTimeoutMs: 60000 });
app.ws("/ws", { message: (c, d, bin) => { lastSrv = d; c.send(d, bin); } });
app.start();
const url = "ws://127.0.0.1:" + app.port + "/ws";

let m0 = 0, sent = 0, got = 0, bad = 0;
const done = await new Promise((resolve) => {
  new WsClient(url, {
    open: (w) => { m0 = rusage().msgsnd; for (; sent < DEPTH; sent++) w.send(frame(sent).buffer); },
    message: (w, d) => {
      if (!same(d, got)) bad++;
      got++;
      if (sent < N) { w.send(frame(sent).buffer); sent++; }
      if (got >= N) { w.close(); resolve(true); }
    },
    close: () => resolve(got >= N),
  });
});
const sends = rusage().msgsnd - m0;

const M = 16;
let tgot = 0, tbad = 0;
await new Promise((resolve) => {
  const t = os.setTimeout(() => resolve(), 10000);
  new WsClient(url, {
    open: (w) => {
      os.setTimeout(() => { for (let k = 0; k < M; k++) w.send(frame(k).buffer); }, 20);
    },
    message: (w, d) => { if (!same(d, tgot)) tbad++; tgot++; if (tgot >= M) { os.clearTimeout(t); w.close(); resolve(); } },
  });
});

lastSrv = null;
let closedAfter = false;
await new Promise((resolve) => {
  const t = os.setTimeout(() => resolve(), 10000);
  new WsClient(url, {
    open: (w) => { w.send(frame(7).buffer); w.close(); },
    close: () => { closedAfter = true; os.setTimeout(() => { os.clearTimeout(t); resolve(); }, 50); },
  });
});
const tail0 = Date.now();
while (lastSrv === null && Date.now() - tail0 < 3000) await new Promise((r) => os.setTimeout(r, 10));

app.close();

__kitS.section("echoed", () => {
ok(done && got === N, "echoed " + got + " of " + N + " messages");
ok(bad === 0, "every echoed byte matches (" + bad + " bad)");
if (sends > 0) ok(sends <= 0.5 * N, `${sends} socket sends for ${N} ${SIZE}-byte messages echoed, ${DEPTH} in flight (want <= ${0.5 * N}: frames of one dispatch share a send)`);
else print("test_ws_client_send_coalesce: msgsnd not counted by this kernel; send-count check skipped");
ok(tgot === M && tbad === 0, "timer-driven sends (outside any dispatch): " + tgot + " of " + M + " echoed intact");
ok(closedAfter && lastSrv !== null && same(lastSrv, 7), "a frame sent in open() right before close() still reaches the server");
print(`test_ws_client_send_coalesce: ${pass} passed, ${fail} failed` + (sends > 0 ? ` (${(sends / N).toFixed(2)} sends per message)` : ""));
if (fail) std.exit(1);
});
__kitS.end();
