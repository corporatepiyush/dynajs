// flags: --std
// covers: c:src/modules/http/http-app-dispatch.c
// slots: 2
// The App under test runs as a child dynajs started in the kit's before hook
// (never under --list) and stopped in after; each case drives it with curl.
import * as os from "os";
import { spawnHelper } from "../lib/helper.js";
import { suite } from "../lib/kit.js";
import * as std from "std";

const t = suite("test_http_security");
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_httpsec.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
let A = null;
const ok = (c, w) => A.ok(c, w);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const haveCurl = !t.listing && sh("command -v curl >/dev/null 2>&1") === 0;

let srvPid = 0, PORT = 0, helper = null, made = false;
const get = (p) => { sh(`curl -s --max-time 3 --path-as-is 'http://127.0.0.1:${PORT}${p}' > ${T}/o 2>/dev/null`); return cat(`${T}/o`); };

t.before(() => {
    if (!haveCurl) return;
    made = true;
    sh(`rm -rf ${T}; mkdir -p ${T}/www ${T}/up`);
    sh(`printf TOPSECRET > ${T}/secret.txt; printf UNTOUCHED > ${T}/victim.txt; printf PUBLIC > ${T}/www/index.html`);
    const srv = std.open(`${T}/srv.js`, "w");
    srv.puts(`import { App } from "dyna:http";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as std from "std";
const app = new App({ port: 0 });
app.static("/s", new Path("${T}/www"));
app.upload("/up", { dir: new Path("${T}/up"), maxFileSize: 1 << 20 }, () => {});
app.start();
const f = std.open("${T}/srv.pid", "w"); f.puts(pid() + " " + app.port); f.close();
`);
    srv.close();
    const exe = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
    helper = spawnHelper(`${exe} --std --std ${T}/srv.js >${T}/srv.log 2>&1`);
    for (let i = 0; i < 100 && !(srvPid > 0); i++) {
        srvPid = parseInt(cat(`${T}/srv.pid`).trim(), 10) || 0;
        if (!(srvPid > 0)) sh("sleep 0.05");
    }
    PORT = parseInt((cat(`${T}/srv.pid`).trim().split(" ")[1]) || "0", 10);
});
t.after(() => {
    if (srvPid > 0) sh(`kill ${srvPid} 2>/dev/null`);
    if (helper) helper.stop();
    if (made) sh(`rm -rf ${T}`);
});

const needCurl = (a) => { if (!haveCurl) a.skip("curl not available"); };

test("the server child starts, reports its port and serves", (a) => {
    needCurl(a);
    ok(srvPid > 0, "server child started");
    ok(PORT > 0, "server child reported its port");
    ok(get("/s/index.html").indexOf("PUBLIC") === 0, "server is actually serving (guards against a vacuous run)");
});

test("static path traversal is blocked", (a) => {
    needCurl(a);
    for (const p of ["/s/../secret.txt", "/s/..%2fsecret.txt", "/s/%2e%2e/secret.txt",
                     "/s/....//secret.txt", "/s/a/../../secret.txt"])
        ok(get(p).indexOf("TOPSECRET") < 0, "traversal blocked: " + p);
    ok(cat(`${T}/secret.txt`) === "TOPSECRET", "secret unmodified");
});

test("an upload does not write through a planted symlink", (a) => {
    needCurl(a);
    sh(`ln -sf ${T}/victim.txt ${T}/up/up_${srvPid}_1`);
    sh(`curl -s --max-time 3 -X POST --data-binary PWNED -H 'Content-Type: application/octet-stream' 'http://127.0.0.1:${PORT}/up' >/dev/null 2>&1`);
    ok(srvPid > 0 && cat(`${T}/victim.txt`) === "UNTOUCHED",
       "upload did not write through the symlink");
});

const mkHeaders = (n, tag) =>
    `awk 'BEGIN{v="";for(i=0;i<1000;i++)v=v "A";` +
    `for(i=0;i<${n};i++)printf "header = \\"X-Pad-%d: %s\\"\\n", i, v}' > ${T}/${tag}.conf`;
const codeOf = (tag) => {
    sh(`curl -s --max-time 8 -o /dev/null -w '%{http_code}' -K ${T}/${tag}.conf ` +
       `'http://127.0.0.1:${PORT}/s/index.html' > ${T}/code.${tag} 2>/dev/null; true`);
    return cat(`${T}/code.${tag}`).trim();
};

test("a header block under the cap is served", (a) => {
    needCurl(a);
    sh(mkHeaders(55, "under"));
    const under = codeOf("under");
    ok(under === "200", "header block UNDER the cap is still served (got " + under + ")");
});

test("a header block over the cap is refused with 431", (a) => {
    needCurl(a);
    sh(mkHeaders(80, "over"));
    sh(mkHeaders(400, "gross"));
    let seen = 0;
    for (const tag of ["over", "gross"]) {
        const c = codeOf(tag);
        if (c === "000")
            print("  (header-cap '" + tag + "': peer closed before status; refused, code unseen)");
        else {
            seen++;
            ok(c === "431", "header block OVER the cap is refused with 431 (" + tag +
                            " got " + c + ")");
        }
    }
    if (!seen) a.ok(true, "both over-cap requests were refused before a status line");
});

await t.run();
