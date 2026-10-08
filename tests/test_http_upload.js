import { App, TCPServer } from "dyna:net";
import { makeTempDir, readDir, readFile, removeAll } from "dyna:file";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const dir = makeTempDir("upl");
const app = new App({ port: 0 });

let lastMeta = null, lastPath = null;
app.upload("/up", { dir: dir, maxFileSize: 4096,
                    allow: ["text/plain", "application/octet-stream"] },
           (saved, meta) => { lastPath = saved; lastMeta = meta; });
app.rpc("/rpc", { ping: () => "pong" });
app.start();
const PORT = app.port;

function raw(headLines, body, cb) {
  const c = TCPServer.connect({ host: "127.0.0.1", port: PORT }, {
    connect: (conn, err) => {
      if (err) { cb(null, String(err)); return; }
      const head = headLines.join("\r\n") + "\r\n\r\n";
      const bytes = new Uint8Array(head.length + body.length);
      for (let i = 0; i < head.length; i++) bytes[i] = head.charCodeAt(i) & 0xff;
      for (let i = 0; i < body.length; i++)
        bytes[head.length + i] = body.charCodeAt(i) & 0xff;
      conn.write(bytes);
    },
    data: (conn, b) => {
      let s = "";
      for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
      cb(s, null); conn.close();
    },
  });
  return c;
}
const status = (r) => (r && /^HTTP\/1\.[01] (\d{3})/.exec(r) || [])[1] || "none";

const R = {};
const KEEP = [];
function run(name, headLines, body) {
  R[name] = "pending";
  KEEP.push(raw(headLines, body, (resp, err) => {
    R[name] = err ? { err } : { resp };
    print("  [" + name + "] " + (err ? "err " + err : (resp || "").split("\r\n")[0]));
  }));
}

const H = (extra, len) => ["POST /up HTTP/1.1", "Host: x",
                           "Content-Length: " + len].concat(extra);

run("ok",        H(["Content-Type: text/plain"], 5), "hello");
run("params",    H(["Content-Type: text/plain; charset=utf-8"], 3), "abc");
run("spaces",    H(["Content-Type: text/plain      "], 3), "abc");
run("longtype",  H(["Content-Type: " + "a".repeat(400)], 3), "abc");
run("edge128",   H(["Content-Type: " + "b".repeat(128)], 3), "abc");
run("denied",    H(["Content-Type: image/png"], 3), "abc");
run("toolarge",  H(["Content-Type: text/plain"], 99999), "");
run("negative",  H(["Content-Type: text/plain"], -1), "");
run("noctype",   ["POST /up HTTP/1.1", "Host: x", "Content-Length: 3"], "abc");
run("short",     H(["Content-Type: text/plain"], 100), "abc");

let spins = 0;
const t = setInterval(() => {
  const pending = Object.keys(R).filter(k => R[k] === "pending").length;
  if (pending && spins++ < 600) return;
  clearInterval(t);
  try {

  const st = (k) => status(R[k] && R[k].resp);
  check(st("ok") === "200", "a legal upload is accepted, got " + st("ok"));
  check(st("params") === "200",
        "a Content-Type with a ;parameter must compare on the type alone, got " + st("params"));
  check(st("spaces") === "200",
        "trailing spaces in the Content-Type are trimmed, got " + st("spaces"));
  check(st("denied") === "415",
        "a type outside `allow` is refused with 415, got " + st("denied"));
  check(st("toolarge") === "413",
        "a Content-Length past maxFileSize is refused, got " + st("toolarge"));
  check(st("longtype") === "415",
        "a 400-byte Content-Type truncates into the buffer and is refused, got " +
        st("longtype"));
  check(st("edge128") === "415",
        "a type exactly the length of the buffer is refused cleanly, got " + st("edge128"));
  check(st("noctype") === "415",
        "no Content-Type at all is refused rather than defaulting in, got " + st("noctype"));
  check(st("negative") === "400",
        "a negative Content-Length is REFUSED, not silently read as 0 -- a front " +
        "end that parses it differently is a smuggling desync, got " + st("negative"));
  check(st("toolarge").indexOf("41") === 0,
        "an over-cap length is refused, got " + st("toolarge"));

  const files = readDir(dir).filter(e => e.name.indexOf("up_") === 0);
  check(files.length >= 3,
        "the three accepted uploads are written to disk, found " + files.length);
  const bodies = files.map(f => readFile(dir.join(f.name)));
  check(bodies.indexOf("hello") >= 0,
        "and the bytes on disk are the bytes sent, got " + JSON.stringify(bodies));
  check(bodies.indexOf("abc".repeat(34)) < 0, "the short upload is not completed");
  check(lastMeta && lastMeta.contentType === "text/plain",
        "the handler's meta reports the PARSED type, got " +
        (lastMeta && lastMeta.contentType));

  for (const k of KEEP) { try { k.close(); } catch (e) {} }
  app.close();
  try { removeAll(dir); } catch (e) {}
  if (fails === 0) print("test_http_upload: all " + n + " checks passed");
  else {
    print("test_http_upload: " + fails + " FAILED of " + n);
    throw new Error("test_http_upload: " + fails + " failures");
  }
  } catch (e) {
    print("test_http_upload: THREW " + e.message + "\n" + (e.stack || ""));
    try { app.close(); removeAll(dir); } catch (e2) {}
    throw e;
  }
}, 10);
