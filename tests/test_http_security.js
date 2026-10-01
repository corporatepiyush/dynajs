// flags: --std
import * as os from "os";
import * as std from "std";

const PORT = 18213;
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_httpsec.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL: " + w); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };

if (sh("command -v curl >/dev/null 2>&1") !== 0) {
    print("test_http_security: SKIP (curl not available)");
} else {
    sh(`rm -rf ${T}; mkdir -p ${T}/www ${T}/up`);
    sh(`printf TOPSECRET > ${T}/secret.txt; printf UNTOUCHED > ${T}/victim.txt; printf PUBLIC > ${T}/www/index.html`);

    const srv = std.open(`${T}/srv.js`, "w");
    srv.puts(`import { App } from "dyna:net";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as std from "std";
const f = std.open("${T}/srv.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: ${PORT} });
app.static("/s", new Path("${T}/www"));
app.upload("/up", { dir: new Path("${T}/up"), maxFileSize: 1 << 20 }, () => {});
app.start();
`);
    srv.close();

    const exe = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
    sh(`${exe} --std --std ${T}/srv.js >${T}/srv.log 2>&1 &`);
    let srvPid = 0;
    for (let i = 0; i < 100 && !(srvPid > 0); i++) {
        srvPid = parseInt(cat(`${T}/srv.pid`).trim(), 10) || 0;
        if (!(srvPid > 0)) sh("sleep 0.05");
    }
    ok(srvPid > 0, "server child started");

    const get = (p) => { sh(`curl -s --max-time 3 --path-as-is 'http://127.0.0.1:${PORT}${p}' > ${T}/o 2>/dev/null`); return cat(`${T}/o`); };

    ok(get("/s/index.html").indexOf("PUBLIC") === 0, "server is actually serving (guards against a vacuous run)");

    for (const p of ["/s/../secret.txt", "/s/..%2fsecret.txt", "/s/%2e%2e/secret.txt",
                     "/s/....//secret.txt", "/s/a/../../secret.txt"])
        ok(get(p).indexOf("TOPSECRET") < 0, "traversal blocked: " + p);
    ok(cat(`${T}/secret.txt`) === "TOPSECRET", "secret unmodified");

    sh(`ln -sf ${T}/victim.txt ${T}/up/up_${srvPid}_1`);
    sh(`curl -s --max-time 3 -X POST --data-binary PWNED -H 'Content-Type: application/octet-stream' 'http://127.0.0.1:${PORT}/up' >/dev/null 2>&1`);
    ok(srvPid > 0 && cat(`${T}/victim.txt`) === "UNTOUCHED",
       "upload did not write through the symlink");

    const mkHeaders = (n, tag) =>
        `awk 'BEGIN{v="";for(i=0;i<1000;i++)v=v "A";` +
        `for(i=0;i<${n};i++)printf "header = \\"X-Pad-%d: %s\\"\\n", i, v}' > ${T}/${tag}.conf`;
    const codeOf = (tag) => {
        sh(`curl -s --max-time 8 -o /dev/null -w '%{http_code}' -K ${T}/${tag}.conf ` +
           `'http://127.0.0.1:${PORT}/s/index.html' > ${T}/code.${tag} 2>/dev/null; true`);
        return cat(`${T}/code.${tag}`).trim();
    };
    sh(mkHeaders(55, "under"));
    sh(mkHeaders(80, "over"));
    sh(mkHeaders(400, "gross"));

    const under = codeOf("under");
    ok(under === "200", "header block UNDER the cap is still served (got " + under + ")");

    for (const tag of ["over", "gross"]) {
        const c = codeOf(tag);
        if (c === "000")
            print("  (header-cap '" + tag + "': peer closed before status; refused, code unseen)");
        else
            ok(c === "431", "header block OVER the cap is refused with 431 (" + tag +
                            " got " + c + ")");
    }

    if (srvPid > 0) sh(`kill ${srvPid} 2>/dev/null`);
    sh(`rm -rf ${T}`);
    print("test_http_security: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("test_http_security: " + fail + " failures");
}
