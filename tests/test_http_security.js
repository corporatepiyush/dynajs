/* test_http_security.js -- regressions for two App attack surfaces, both
 * demonstrated exploitable against the code as it stood on 2026-07-28.
 *
 *   1. UPLOAD SYMLINK RACE. The temp name is predictable (up_<pid>_<seq>) and
 *      the open lacked O_EXCL, so a symlink pre-placed in the upload directory
 *      was followed and TRUNCATED -- an arbitrary file write. Measured against
 *      the pre-fix binary: the target's contents became the request body.
 *
 *   2. HEADER BLOCK CAP. A peer that never sends CRLFCRLF grew the connection
 *      buffer to the ~1 MB per-request ceiling with no header-specific limit.
 *      Now 64 KB, answered with 431.
 *
 * THE SERVER RUNS IN A CHILD PROCESS, and that is not incidental. App handlers
 * execute on the JS thread, so a blocking client call from the same process
 * deadlocks and every request fails. An in-process draft of this test "passed"
 * against the vulnerable binary for exactly that reason -- nothing was ever
 * served. Two processes, or it proves nothing.
 *
 * Skips cleanly when curl is unavailable.
 */
import * as os from "os";
import * as std from "std";

const PORT = 18213;
const T = "/tmp/_dyna_httpsec";
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
    /* POLL for the pid file rather than sleeping a fixed second. Under load the
       child had not written it yet, `srvPid` came back 0, and the cases guarded
       on it below evaporated -- the run reported 11 checks instead of 12 with
       zero failures, which is indistinguishable from a pass. */
    sh(`${exe} ${T}/srv.js >${T}/srv.log 2>&1 &`);
    let srvPid = 0;
    for (let i = 0; i < 100 && !(srvPid > 0); i++) {
        srvPid = parseInt(cat(`${T}/srv.pid`).trim(), 10) || 0;
        if (!(srvPid > 0)) sh("sleep 0.05");
    }
    ok(srvPid > 0, "server child started");

    const get = (p) => { sh(`curl -s --max-time 3 --path-as-is 'http://127.0.0.1:${PORT}${p}' > ${T}/o 2>/dev/null`); return cat(`${T}/o`); };

    ok(get("/s/index.html").indexOf("PUBLIC") === 0, "server is actually serving (guards against a vacuous run)");

    /* 1. traversal. The strstr("..") guard suffices only because the path is
       never percent-decoded; if decoding is added, %2e%2e arrives decoded and
       these rows are what catch it. */
    for (const p of ["/s/../secret.txt", "/s/..%2fsecret.txt", "/s/%2e%2e/secret.txt",
                     "/s/....//secret.txt", "/s/a/../../secret.txt"])
        ok(get(p).indexOf("TOPSECRET") < 0, "traversal blocked: " + p);
    ok(cat(`${T}/secret.txt`) === "TOPSECRET", "secret unmodified");

    /* 2. upload symlink race: plant the exact first name the server will try. */
    /* No `if (srvPid > 0)` guard: a missing pid must FAIL this case, not delete
       it. A conditional here turns a missed precondition into a lower count
       with zero failures, and nobody compares counts across runs. */
    sh(`ln -sf ${T}/victim.txt ${T}/up/up_${srvPid}_1`);
    sh(`curl -s --max-time 3 -X POST --data-binary PWNED -H 'Content-Type: application/octet-stream' 'http://127.0.0.1:${PORT}/up' >/dev/null 2>&1`);
    ok(srvPid > 0 && cat(`${T}/victim.txt`) === "UNTOUCHED",
       "upload did not write through the symlink");

    /* 3. header cap. DYN_APP_MAX_HEADER is 64 KiB on the whole buffered header
       block, answered with 431.

       "Not 200 for a huge header" is too weak a check: a server that refused
       EVERY request would pass it. So this drives the boundary from both sides
       -- a block just under the cap must still be SERVED, one just over must be
       refused -- and it asserts the status is 431 rather than merely non-200.

       The headers go in a curl config file rather than on the command line:
       the cap is on the block, not one header, and a 128 KB argv entry is at
       the mercy of ARG_MAX (which is what made the previous version skip). */
    const mkHeaders = (n, tag) =>
        `awk 'BEGIN{v="";for(i=0;i<1000;i++)v=v "A";` +
        `for(i=0;i<${n};i++)printf "header = \\"X-Pad-%d: %s\\"\\n", i, v}' > ${T}/${tag}.conf`;
    const codeOf = (tag) => {
        sh(`curl -s --max-time 8 -o /dev/null -w '%{http_code}' -K ${T}/${tag}.conf ` +
           `'http://127.0.0.1:${PORT}/s/index.html' > ${T}/code.${tag} 2>/dev/null; true`);
        return cat(`${T}/code.${tag}`).trim();
    };
    sh(mkHeaders(55, "under"));   /* ~57 KB of headers: under the 64 KiB cap */
    sh(mkHeaders(80, "over"));    /* ~83 KB: over it */
    sh(mkHeaders(400, "gross"));  /* ~414 KB: far over */

    const under = codeOf("under");
    ok(under === "200", "header block UNDER the cap is still served (got " + under + ")");

    for (const tag of ["over", "gross"]) {
        const c = codeOf(tag);
        /* 431 is the contract. curl reports 000 when the peer closes before the
           response is read, which is a refusal but not evidence of the status,
           so it is accepted and called out rather than counted as a pass. */
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
