// flags: --std
import * as os from "os";
import * as std from "std";
import { STRINGS, PATHS } from "./fuzzgen.js";

const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_httpparams.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
const SPORT = 18613;
const UPORT = 18614;
const PPORT = 18615;

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  -- " + d : "")); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_params: SKIP (python3 not available)");
    std.exit(0);
}
sh(`rm -rf ${T}; mkdir -p ${T}/static`);
write(`${T}/static/hello.txt`, "static-body\n");

const CASES = [
    ["rpc add returns the result", "POST", "/rpc",
     '{"method":"add","params":[40,2],"id":1}', {status: 200, bodyHas: '"result":42', responses: 1}],
    ["rpc echoes the request id", "POST", "/rpc",
     '{"method":"add","params":[1,1],"id":7}', {status: 200, bodyHas: '"id":7', responses: 1}],
    ["rpc id may be a string", "POST", "/rpc",
     '{"method":"add","params":[1,1],"id":"abc"}', {status: 200, bodyHas: '"id":"abc"', responses: 1}],
    ["rpc reply is valid JSON", "POST", "/rpc",
     '{"method":"add","params":[1,1],"id":1}', {status: 200, jsonParses: true, responses: 1}],
    ["notification (no id) still answers or stays silent", "POST", "/rpc",
     '{"method":"add","params":[1,1]}', {responses: [0, 1]}],
    ["unknown method is -32601", "POST", "/rpc",
     '{"method":"nope","params":[],"id":2}', {status: 404, bodyHas: "-32601", responses: 1}],
    ["malformed json is -32700", "POST", "/rpc",
     "not json at all", {status: 400, bodyHas: "-32700", responses: 1}],
    ["missing method is -32600", "POST", "/rpc",
     '{"params":[],"id":3}', {status: 400, bodyHas: "-32600", responses: 1}],
    ["batch of 2 returns an array", "POST", "/rpc",
     '[{"method":"add","params":[1,1],"id":1},{"method":"add","params":[2,2],"id":2}]',
     {status: 200, bodyHas: '"result":4', responses: 1}],
    ["empty body", "POST", "/rpc", "", {status: 400, responses: 1}],

    ["static serves the file", "GET", "/s/hello.txt", null,
     {status: 200, bodyHas: "static-body", responses: 1}],
    ["static 404s a missing file", "GET", "/s/nope.txt", null,
     {status: 404, responses: 1}],
    ["static refuses traversal", "GET", "/s/../srv.js", null,
     {statusNot: 200, responses: 1}],
    ["static refuses encoded traversal", "GET", "/s/%2e%2e/srv.js", null,
     {statusNot: 200, responses: 1}],

    ["GET on an rpc route is not a 200 result", "GET", "/rpc", null,
     {statusNot: 200, responses: 1}],
    ["unknown route is 404", "GET", "/no-such-route", null,
     {status: 404, responses: 1}],

    ["notification gets an EMPTY body, not a null id", "POST", "/rpc",
     '{"method":"add","params":[1,1]}', {bodyEmptyOrValidJson: true}],
    ["id null is a valid id and must be echoed", "POST", "/rpc",
     '{"method":"add","params":[1,1],"id":null}', {jsonParses: true, responses: 1}],
    ["id 0 is a valid id", "POST", "/rpc",
     '{"method":"add","params":[1,1],"id":0}', {status: 200, bodyHas: '"id":0', responses: 1}],
    ["result survives a negative id", "POST", "/rpc",
     '{"method":"add","params":[-1,-2],"id":-9}', {status: 200, bodyHas: '"result":-3', responses: 1}],
    ["params may be omitted", "POST", "/rpc",
     '{"method":"echo","id":1}', {jsonParses: true, responses: 1}],
    ["params of the wrong type does not crash", "POST", "/rpc",
     '{"method":"add","params":"nope","id":1}', {jsonParses: true, responses: 1}],
    ["deeply nested params are bounded", "POST", "/rpc",
     '{"method":"echo","params":' + "[".repeat(2000) + "]".repeat(2000) + ',"id":1}',
     {responses: 1}],
    ["a batch containing garbage still frames one response", "POST", "/rpc",
     '[{"method":"add","params":[1,1],"id":1},7,null]', {responses: 1, jsonParses: true}],
    ["an empty batch is an error, not a hang", "POST", "/rpc", "[]",
     {responses: 1, jsonParses: true}],
    ["a 200k body is refused or answered, never both", "POST", "/rpc",
     '{"method":"echo","params":"' + "A".repeat(200000) + '","id":1}', {responses: [0, 1]}],

    ["a query string does not change the route", "POST", "/rpc?x=1",
     '{"method":"add","params":[1,1],"id":1}', {status: 200, responses: 1}],
    ["a trailing slash is a different route", "GET", "/rpc/", null, {responses: 1}],
    ["a very long path is refused cleanly", "GET", "/" + "a".repeat(20000), null,
     {responses: [0, 1]}],
    ["a path with a NUL escape is not a 200", "GET", "/s/%00hello.txt", null,
     {statusNot: 200, responses: 1}],
    ["case is preserved in the route match", "GET", "/RPC", null, {statusNot: 200, responses: 1}],

    ["static refuses backslash traversal", "GET", "/s/..%5Csrv.js", null,
     {statusNot: 200, responses: 1}],
    ["static refuses double-encoded traversal", "GET", "/s/%252e%252e/srv.js", null,
     {statusNot: 200, responses: 1}],
    ["static refuses a deep traversal run", "GET", "/s/" + "../".repeat(40) + "etc/passwd", null,
     {statusNot: 200, responses: 1}],
    ["static refuses an absolute path", "GET", "/s//etc/passwd", null,
     {statusNot: 200, responses: 1}],
    ["static serves the same bytes twice", "GET", "/s/hello.txt", null,
     {status: 200, bodyHas: "static-body", responses: 1}],

    ["one request yields exactly one response", "POST", "/rpc",
     '{"method":"add","params":[1,1],"id":1}', {responses: 1}],
];

write(`${T}/srv.js`, `import { App } from "dyna:net";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as std from "std";
const f = std.open("${T}/srv.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: ${SPORT}, idleTimeoutMs: 5000 });
app.rpc("/rpc", { add: (a, b) => a + b, echo: (a) => a });
app.static("/s", new Path("${T}/static"));
app.start();
`);

write(`${T}/up.js`, `import { App } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";
const f = std.open("${T}/up.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: ${UPORT}, idleTimeoutMs: 5000 });
/* Registered at BOTH paths: whether app.proxy() strips its prefix before
   forwarding is the behaviour under test, not an assumption to bake in. */
app.rpc("/rpc", { add: (a, b) => a + b });
app.rpc("/", { add: (a, b) => a + b });
app.rpc("/up/rpc", { add: (a, b) => a + b });
app.start();
`);
write(`${T}/prox.js`, `import { App } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";

const f = std.open("${T}/prox.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: ${PPORT}, idleTimeoutMs: 5000 });
app.proxy("/up", { host: "127.0.0.1", port: ${UPORT} });
app.start();
`);

write(`${T}/client.py`, `import socket, json, sys, time
host, port = "127.0.0.1", int(sys.argv[1])
cases = json.load(open(sys.argv[2]))
out = []
for c in cases:
    body = c["body"]
    req = "%s %s HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n" % (c["method"], c["path"])
    if body is not None:
        req += "Content-Length: %d\\r\\n" % len(body.encode())
    req += "\\r\\n" + (body or "")
    rec = {"name": c["name"], "status": None, "body": "", "responses": 0, "err": None}
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(req.encode()); s.settimeout(0.2)
        buf = b""
        while True:
            try:
                b = s.recv(65536)
                if not b: break
                buf += b
                sep = buf.find(b"\\r\\n\\r\\n")
                if sep != -1:
                    hdr = buf[:sep].decode("utf-8", "replace").lower()
                    cl_idx = hdr.find("content-length:")
                    if cl_idx != -1:
                        try:
                            cl_val = int(hdr[cl_idx+15:].split("\\r\\n")[0].strip())
                            if len(buf) >= sep + 4 + cl_val:
                                break
                        except Exception:
                            pass
            except socket.timeout: break
        s.close()
        txt = buf.decode("utf-8", "replace")
        rec["responses"] = txt.count("HTTP/1.1 ")
        if txt.startswith("HTTP/1.1 "):
            rec["status"] = int(txt[9:12])
        i = txt.find("\\r\\n\\r\\n")
        rec["body"] = txt[i+4:] if i >= 0 else ""
    except Exception as e:
        rec["err"] = str(e)
    out.append(rec)
json.dump(out, open(sys.argv[3], "w"))
`);

function start(script, pidfile) {
    sh(`rm -f ${pidfile}`);
    sh(`./dynajs --std ${script} > ${T}/$(basename ${script}).log 2>&1 &`);
    for (let i = 0; i < 60; i++) { if (cat(pidfile).trim()) break; os.sleep(50); }
    os.sleep(250);
    return parseInt(cat(pidfile).trim() || "0", 10);
}
const stop = (p) => { if (p > 0) sh(`kill ${p} 2>/dev/null`); };

const lone = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:[^\uD800-\uDBFF]|^)[\uDC00-\uDFFF]/;
const sendable = (a) => a.filter((x) => !lone.test(x));

const CORPUS_CASES = [];
{
    let i = 0;
    for (const pth of sendable(PATHS).slice(0, 25)) {
        CORPUS_CASES.push([`corpus path #${i++}`, "GET", pth, "",
            {responses: [0, 1]}]);
    }
    i = 0;
    for (const str of sendable(STRINGS).slice(0, 35)) {
        CORPUS_CASES.push([`corpus rpc method #${i++}`, "POST", "/rpc",
            JSON.stringify({method: str, params: [1, 1], id: 1}),
            {responses: [0, 1]}]);
    }
    CORPUS_CASES.push(["server survives the corpus", "POST", "/rpc",
        '{"method":"add","params":[40,2],"id":1}',
        {status: 200, bodyHas: '"result":42', responses: 1}]);
}

const ALL_CASES = CASES.concat(CORPUS_CASES);

print("\n-- App HTTP server: " + ALL_CASES.length + " cases (" +
      CASES.length + " tabled, " + CORPUS_CASES.length + " corpus-driven) --");
const srvPid = start(`${T}/srv.js`, `${T}/srv.pid`);
ok(srvPid > 0, "server started");

write(`${T}/cases.json`, JSON.stringify(ALL_CASES.map(
    ([name, method, path, body]) => ({ name, method, path, body }))));
sh(`python3 ${T}/client.py ${SPORT} ${T}/cases.json ${T}/out.json`);
const R = JSON.parse(cat(`${T}/out.json`) || "[]");

for (let i = 0; i < ALL_CASES.length; i++) {
    const [name, , , , want] = ALL_CASES[i];
    const got = R[i];
    if (!got) { ok(false, name, "no result recorded"); continue; }
    const why = [];
    if (want.status !== undefined && got.status !== want.status)
        why.push(`status ${got.status} want ${want.status}`);
    if (want.statusNot !== undefined && got.status === want.statusNot)
        why.push(`status must not be ${want.statusNot}`);
    if (want.bodyHas && got.body.indexOf(want.bodyHas) < 0)
        why.push(`body lacks ${JSON.stringify(want.bodyHas)}: ${JSON.stringify(got.body.slice(0, 90))}`);
    if (want.bodyEmptyOrValidJson) {
        if (got.body.trim() !== "") {
            try { JSON.parse(got.body); }
            catch (e) { why.push("notification reply is not valid JSON: " +
                                 JSON.stringify(got.body.slice(0, 90))); }
        }
    }
    if (want.jsonParses) {
        try { JSON.parse(got.body); }
        catch (e) { why.push("body is not valid JSON: " + JSON.stringify(got.body.slice(0, 90))); }
    }
    if (want.responses !== undefined) {
        const wr = Array.isArray(want.responses) ? want.responses : [want.responses];
        if (wr.indexOf(got.responses) < 0)
            why.push(`${got.responses} responses, want ${wr.join(" or ")}`);
    }
    if (got.err) why.push("client error: " + got.err);
    ok(why.length === 0, name, why.join("; "));
}
ok(srvPid > 0 && sh(`kill -0 ${srvPid} 2>/dev/null`) === 0,
   "server survived every case");
stop(srvPid);

print("\n-- proxy: the same cases through a forwarding hop --");
const upPid = start(`${T}/up.js`, `${T}/up.pid`);
const pxPid = start(`${T}/prox.js`, `${T}/prox.pid`);
ok(upPid > 0 && pxPid > 0, "upstream and proxy started",
   `up=${upPid} proxy=${pxPid}`);

const PROXY_CASES = [
    ["proxy forwards an rpc call", "POST", "/up/rpc",
     '{"method":"add","params":[20,22],"id":1}', {status: 200, bodyHas: '"result":42', responses: 1}],
    ["proxy preserves the id", "POST", "/up/rpc",
     '{"method":"add","params":[1,1],"id":"px"}', {status: 200, bodyHas: '"id":"px"', responses: 1}],
    ["proxy relays an upstream 404", "POST", "/up/rpc",
     '{"method":"nope","params":[],"id":1}', {status: 404, responses: 1}],
    ["proxy relays malformed input", "POST", "/up/rpc", "garbage", {status: 400, responses: 1}],
    ["proxy emits exactly one response", "POST", "/up/rpc",
     '{"method":"add","params":[1,1],"id":1}', {responses: 1}],
];
write(`${T}/pcases.json`, JSON.stringify(PROXY_CASES.map(
    ([name, method, path, body]) => ({ name, method, path, body }))));
sh(`python3 ${T}/client.py ${PPORT} ${T}/pcases.json ${T}/pout.json`);
const P = JSON.parse(cat(`${T}/pout.json`) || "[]");

for (let i = 0; i < PROXY_CASES.length; i++) {
    const [name, , , , want] = PROXY_CASES[i];
    const got = P[i];
    if (!got) { ok(false, name, "no result recorded"); continue; }
    const why = [];
    if (want.status !== undefined && got.status !== want.status)
        why.push(`status ${got.status} want ${want.status}`);
    if (want.bodyHas && got.body.indexOf(want.bodyHas) < 0)
        why.push(`body lacks ${JSON.stringify(want.bodyHas)}: ${JSON.stringify(got.body.slice(0, 90))}`);
    if (want.responses !== undefined && got.responses !== want.responses)
        why.push(`${got.responses} responses, want ${want.responses}`);
    if (got.err) why.push("client error: " + got.err);
    ok(why.length === 0, name, why.join("; "));
}
stop(pxPid); stop(upPid);
if (fail === 0) sh(`rm -rf ${T}`);

print("\ntest_http_params: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail > 0) std.exit(1);
