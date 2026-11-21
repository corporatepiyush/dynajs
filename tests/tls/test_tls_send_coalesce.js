// flags: --std
// timeout: 60
// covers: c:src/modules/net-tcp/net-tcp-tls.c
// TCPServer/TCPServer.connect over TLS: one write() of up to 16 KiB is one TLS record, and its
// ciphertext (record + 22 bytes of header and tag) must leave in ONE send. tls_flush pulled through a
// 16384-byte buffer, so every full record went out as two sends (16384 + 22), and every write was
// sent at once instead of joining the other writes of the same event-loop dispatch. Now the pull
// buffer holds a whole record and the sends are held (DYN_AIO_HOLD) until the dispatch ends.
// Measured with getrusage's msgsnd (socket messages sent), which macOS counts; where the kernel
// leaves it 0 (Linux) only the data checks run. An echo of N 16 KiB messages, 8 in flight, both ends
// in this process, sent 4N before, 2N with the whole-record buffer alone, ~0.7N with the hold as
// well; the bound is 1.5N. Writes made from a timer run outside any I/O dispatch, where nothing is
// held: there each 16 KiB record must still be one send, not two (the whole-record buffer's half).
// Every echoed byte is checked.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_tls_send_coalesce");
import * as std from "std";
import * as os from "os";
import { TCPServer } from "dyna:net";
import { Exec, Which, rusage } from "dyna:sys";
import { makeTempDir, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

if (!Which("openssl")) {
  print("test_tls_send_coalesce: SKIP (openssl(1) missing to make a certificate)");
  std.exit(0);
}
const T = String(makeTempDir("tlscoal"));
Exec("/bin/sh", ["-c", `openssl req -x509 -newkey rsa:2048 -keyout ${T}/key.pem -out ${T}/cert.pem -days 2 -nodes -subj "/CN=localhost" 2>/dev/null`]);

const SIZE = 16384, N = 400, DEPTH = 8;
const msg = new Uint8Array(SIZE);
for (let i = 0; i < SIZE; i++) msg[i] = (i * 37 + 11) & 255;

const srv = new TCPServer({ port: 0, tls: { cert: T + "/cert.pem", key: T + "/key.pem" } });
srv.start({ data: (c, b) => c.write(b) });

let msgsnd0 = 0, sent = 0, recvd = 0, bad = 0;
const done = await new Promise((resolve) => {
  TCPServer.connect({ host: "127.0.0.1", port: srv.port, tls: { rejectUnauthorized: false } }, {
    connect: (c) => {
      if (!c) return resolve(false);
      msgsnd0 = rusage().msgsnd;
      for (; sent < DEPTH; sent++) c.write(msg);
    },
    data: (c, b) => {
      for (let i = 0; i < b.length; i++) if (b[i] !== msg[(recvd + i) % SIZE]) { bad++; break; }
      recvd += b.length;
      while (sent < N && recvd + DEPTH * SIZE > sent * SIZE) { c.write(msg); sent++; }
      if (recvd >= N * SIZE) { c.close(); resolve(true); }
    },
    close: () => resolve(recvd >= N * SIZE),
  });
});
const sends = rusage().msgsnd - msgsnd0;
srv.close();

const M = 8;
const sink = new TCPServer({ port: 0, tls: { cert: T + "/cert.pem", key: T + "/key.pem" } });
let sunk = 0, timerSends = -1;
sink.start({ data: (c, b) => { sunk += b.length; } });
await new Promise((resolve) => {
  TCPServer.connect({ host: "127.0.0.1", port: sink.port, tls: { rejectUnauthorized: false } }, {
    connect: (c) => {
      if (!c) return resolve();
      os.setTimeout(() => {
        const m0 = rusage().msgsnd;
        for (let i = 0; i < M; i++) c.write(msg);
        timerSends = rusage().msgsnd - m0;
        const poll = os.setInterval(() => { if (sunk >= M * SIZE) { os.clearInterval(poll); c.close(); resolve(); } }, 5);
      }, 20);
    },
  });
});
sink.close();
removeAll(new Path(T));

__kitS.section("echoed", () => {
ok(done && recvd === N * SIZE, "echoed " + recvd + " of " + N * SIZE + " bytes");
ok(bad === 0, "every echoed byte matches");
if (sends > 0) ok(sends <= 1.5 * N, `${sends} socket sends for ${N} 16 KiB messages echoed (want <= ${1.5 * N}: under one per record per side)`);
else print("test_tls_send_coalesce: msgsnd not counted by this kernel; send-count check skipped");
ok(sunk === M * SIZE, "timer-driven writes: " + sunk + " of " + M * SIZE + " bytes arrived");
if (sends > 0) ok(timerSends >= 0 && timerSends <= 1.2 * M, `${timerSends} sends for ${M} 16 KiB writes made from a timer, outside any I/O dispatch (no hold applies; want <= ${1.2 * M}: one per record)`);
print(`test_tls_send_coalesce: ${pass} passed, ${fail} failed` + (sends > 0 ? ` (${(sends / N).toFixed(2)} sends per message)` : ""));
if (fail) std.exit(1);
});
__kitS.end();
