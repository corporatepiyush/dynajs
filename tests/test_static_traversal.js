/* test_static_traversal.js -- App.static must not serve outside its root.
 *
 * WHY: dyna:file and the static handler had NO adversarial coverage, and the
 * handler's guard was `strstr(sub, "..")` under a comment claiming
 * "Path-traversal-proof". A substring test cannot see a SYMLINK -- a link
 * inside the served directory pointing outside contains no dots at all, and
 * stat() follows it. Measured before the fix: /s/link/creds.txt returned 200
 * with the contents of a file outside the root.
 *
 * THE ORACLE IS A CANARY, not a status code. A 403 could mean the file was
 * missing, the type was filtered, or the request never resolved; only "did the
 * secret's CONTENT come back" distinguishes contained from merely unlucky.
 *
 * The server runs in its OWN process: Exec() is synchronous and blocks the
 * event loop the server needs.
 */
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
const out = (c) => (Exec("/bin/sh", ["-c", c]).stdout || "");

const CANARY = "TOP-SECRET-CANARY";

if (!Which("curl")) {
    skipped("curl missing -- --path-as-is is needed to send a raw target");
    print("test_static_traversal: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_static_traversal: " + fail + " failures");
} else {
    const T = makeTempDir("statictrav");
    const P = (n) => T + "/" + n;
    sh(`mkdir -p ${P("www")} ${P("secret")}`);
    sh(`printf 'PUBLIC' > ${P("www/ok.txt")}`);
    sh(`printf '${CANARY}' > ${P("secret/creds.txt")}`);
    /* Both shapes: a link to the DIRECTORY and a link to the FILE. They fail
       differently -- the first escapes during traversal, the second at the
       leaf -- and an implementation can stop one without stopping the other. */
    sh(`ln -s ${P("secret")} ${P("www/link")}`);
    sh(`ln -s ${P("secret/creds.txt")} ${P("www/direct.txt")}`);

    writeFile(new Path(P("srv.js")), `
import { App } from "dyna:net";
import { Path } from "dyna:file";
import * as std from "std";
const app = new App({ port: 0 });
app.static("/s", new Path(${JSON.stringify(P("www"))}));
app.start();
const f = std.open(${JSON.stringify(P("port"))}, "w"); f.puts(String(app.port)); f.close();
setTimeout(() => app.close(), 40000);
`);
    sh(`./dynajs ${P("srv.js")} >/dev/null 2>&1 &`);
    let port = 0;
    for (let i = 0; i < 60 && !port; i++) {
        sh("sleep 0.2");
        port = parseInt(out(`cat ${P("port")} 2>/dev/null`), 10) || 0;
    }
    if (!port) skipped("the static server child did not start");
    else {
        const get = (target) =>
            out(`curl -s --path-as-is -m 4 "http://127.0.0.1:${port}${target}" 2>/dev/null`);

        /* CONTROL FIRST. Without it, every "safe" below could just mean the
           server was not serving anything at all. */
        ok(get("/s/ok.txt").indexOf("PUBLIC") >= 0,
           "CONTROL: a legitimate file under the root IS served");

        const ESCAPES = [
            ["a plain ../ escape", "/s/../secret/creds.txt"],
            ["a percent-encoded ../", "/s/%2e%2e/secret/creds.txt"],
            ["a double percent-encoded ../", "/s/%252e%252e/secret/creds.txt"],
            ["a backslash variant", "/s/..\\secret\\creds.txt"],
            ["a SYMLINK to a directory outside the root", "/s/link/creds.txt"],
            ["a SYMLINK straight to the file", "/s/direct.txt"],
            ["an absolute path in the target", "/s//" + P("secret/creds.txt")],
        ];
        for (const [what, target] of ESCAPES)
            ok(get(target).indexOf(CANARY) < 0,
               what + " does not leak the file's CONTENT");

        /* The containment check must not break ordinary nested serving. */
        sh(`mkdir -p ${P("www/sub")} && printf 'NESTED' > ${P("www/sub/deep.txt")}`);
        ok(get("/s/sub/deep.txt").indexOf("NESTED") >= 0,
           "CONTROL: a nested file under the root is still served");
    }
    sh(`pkill -f ${JSON.stringify(P("srv.js"))} 2>/dev/null`);
    removeAll(new Path(T));
    print("test_static_traversal: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_static_traversal: " + fail + " failures");
}
