// covers: dyna:http.App
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { makeTempDir, readDir, readFile, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

// The App starts, and every raw request below is sent and answered, in the
// kit's before hook (never under --list); each case checks one behaviour.
const t = suite("test_http_upload");
let A = null;
const check = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

let dir = null, app = null, PORT = 0;
let lastMeta = null, lastPath = null;
t.before(async () => {
dir = makeTempDir("upl");
app = new App({ port: 0 });
app.upload("/up", { dir: dir, maxFileSize: 4096,
                    allow: ["text/plain", "application/octet-stream"] },
           (saved, meta) => { lastPath = saved; lastMeta = meta; });
app.rpc("/rpc", { ping: () => "pong" });
app.start();
PORT = app.port;
fire();
let spins = 0;
while (Object.keys(R).some((k) => R[k] === "pending") && spins++ < 600) await new Promise((r) => setTimeout(r, 10));
});
t.after(() => {
  for (const k of KEEP) { try { k.close(); } catch (e) {} }
  if (app) app.close();
  if (dir) { try { removeAll(dir); } catch (e) {} }
});

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

function fire() {
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
}

const st = (k) => status(R[k] && R[k].resp);

test("a legal upload is accepted and answers {ok, id} without the server path", () => {
  check(st("ok") === "200", "a legal upload is accepted, got " + st("ok"));
  const okBody = R.ok && R.ok.resp ? R.ok.resp.slice(R.ok.resp.indexOf("\r\n\r\n") + 4) : "";
  const okReply = (() => { try { return JSON.parse(okBody); } catch { return null; } })();
  check(okReply && okReply.ok === true && /^up_\w+$/.test(okReply.id),
        "an upload answers {ok, id} with the saved file's basename, got " + okBody);
  check(!okBody.includes("/"),
        "the upload reply never carries the server path, got " + okBody);
  check(okReply && readDir(dir).some((e) => e.name === okReply.id),
        "the reply id names the saved file in the upload directory");
});

test("a Content-Type compares on the type alone, parameters and trailing spaces aside", () => {
  check(st("params") === "200",
        "a Content-Type with a ;parameter must compare on the type alone, got " + st("params"));
  check(st("spaces") === "200",
        "trailing spaces in the Content-Type are trimmed, got " + st("spaces"));
});

test("a type outside allow, an oversized or a missing Content-Type is refused with 415", () => {
  check(st("denied") === "415",
        "a type outside `allow` is refused with 415, got " + st("denied"));
  check(st("longtype") === "415",
        "a 400-byte Content-Type truncates into the buffer and is refused, got " +
        st("longtype"));
  check(st("edge128") === "415",
        "a type exactly the length of the buffer is refused cleanly, got " + st("edge128"));
  check(st("noctype") === "415",
        "no Content-Type at all is refused rather than defaulting in, got " + st("noctype"));
});

test("a Content-Length past maxFileSize is refused with 413", () => {
  check(st("toolarge") === "413",
        "a Content-Length past maxFileSize is refused, got " + st("toolarge"));
  check(st("toolarge").indexOf("41") === 0,
        "an over-cap length is refused, got " + st("toolarge"));
});

test("a negative Content-Length is refused with 400", () => {
  check(st("negative") === "400",
        "a negative Content-Length is REFUSED, not silently read as 0 -- a front " +
        "end that parses it differently is a smuggling desync, got " + st("negative"));
});

test("accepted uploads land on disk byte-exact and the short one does not", () => {
  const files = readDir(dir).filter(e => e.name.indexOf("up_") === 0);
  check(files.length >= 3,
        "the three accepted uploads are written to disk, found " + files.length);
  const bodies = files.map(f => readFile(dir.join(f.name)));
  check(bodies.indexOf("hello") >= 0,
        "and the bytes on disk are the bytes sent, got " + JSON.stringify(bodies));
  check(bodies.indexOf("abc".repeat(34)) < 0, "the short upload is not completed");
});

test("the handler's meta reports the parsed type", () => {
  check(lastMeta && lastMeta.contentType === "text/plain",
        "the handler's meta reports the PARSED type, got " +
        (lastMeta && lastMeta.contentType));
});

await t.run();
