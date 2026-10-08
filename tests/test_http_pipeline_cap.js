// flags: --std
import { HTTPServerAsync, TCPServer } from "dyna:net";


let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function latin1(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
}
function bytes(s) {
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
  return a;
}

const BODY = "p".repeat(256 * 1024);
const NREQ = 200;           // 200 * 256 KB = 51 MB of responses >> the 4 MB cap
const REQ = "GET /b HTTP/1.1\r\nHost: a\r\n\r\n";

{
  const srv = new HTTPServerAsync({ port: 0, idleTimeoutMs: 5000,
                                    routes: { "/b": { status: 200,
                                                      contentType: "text/plain",
                                                      body: BODY } } });
  srv.start();

  let buf = "";
  let closed = false;
  let responses = 0;
  let bodies = 0;
  let done = null;
  const finished = new Promise(r => { done = r; });

  let conn = null;
  const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
    connect: (c, err) => {
      if (err) { print("FAIL: connect: " + err); closed = true; done(); return; }
      conn = c;
      c.write(bytes(REQ.repeat(NREQ)));
    },
    data: (c, b) => {
      buf += latin1(b);
      for (;;) {
        const headEnd = buf.indexOf("\r\n\r\n");
        if (headEnd < 0) break;
        const m = /Content-Length: (\d+)/i.exec(buf.slice(0, headEnd));
        if (!m) { fail = fail + 1; print("FAIL: response head lacks Content-Length"); closed = true; done(); return; }
        const total = headEnd + 4 + Number(m[1]);
        if (buf.length < total) break;
        responses++;
        if (buf.substr(headEnd + 4, Number(m[1])) === BODY) bodies++;
        buf = buf.slice(total);
        if (responses === NREQ) { done(); return; }
      }
    },
    close: () => { closed = true; done(); },
  });

  const timeout = new Promise((r) => setTimeout(() => r("timeout"), 30000));
  const outcome = await Promise.race([finished.then(() => "done"), timeout]);

  ok(outcome === "done", "the batch completed without the idle timeout (outcome=" + outcome + ")");
  ok(responses === NREQ, "all " + NREQ + " pipelined responses arrived (got " + responses + ")");
  ok(bodies === NREQ, "and every body is intact (got " + bodies + ")");
  ok(!closed, "the connection survived the batch");
  if (conn) conn.close();
  cli.close();
  srv.close();
}

print("test_http_pipeline_cap: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_pipeline_cap failed");
