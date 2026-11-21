// flags: --std
// covers: dyna:http.App
// slots: 2
// The static App runs as a child dynajs started in the kit's before hook (never
// under --list); one case per escape shape, each asking curl for a raw target.
import { Exec, Which, getEnv } from "dyna:sys";
import { spawnHelper } from "../lib/helper.js";
import { makeTempDir, writeFile, removeAll, Path } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_static_traversal");
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
let A = null;
const ok = (c, w, d) => A.ok(c, w + (d ? "  [" + d + "]" : ""));
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
const out = (c) => (Exec("/bin/sh", ["-c", c]).stdout || "");

const CANARY = "TOP-SECRET-CANARY";
let T = null, port = 0, why = null, helper = null;
const P = (n) => T + "/" + n;
const get = (target) =>
    out(`curl -s --path-as-is -m 4 "http://127.0.0.1:${port}${target}" 2>/dev/null`);

t.before(() => {
    if (!Which("curl")) { why = "curl missing -- --path-as-is is needed to send a raw target"; return; }
    T = makeTempDir("statictrav");
    sh(`mkdir -p ${P("www")} ${P("secret")}`);
    sh(`printf 'PUBLIC' > ${P("www/ok.txt")}`);
    sh(`printf '${CANARY}' > ${P("secret/creds.txt")}`);
    sh(`ln -s ${P("secret")} ${P("www/link")}`);
    sh(`ln -s ${P("secret/creds.txt")} ${P("www/direct.txt")}`);
    writeFile(new Path(P("srv.js")), `
import { App } from "dyna:http";
import { Path } from "dyna:file";
import * as std from "std";
const app = new App({ port: 0 });
app.static("/s", new Path(${JSON.stringify(P("www"))}));
app.start();
const f = std.open(${JSON.stringify(P("port"))}, "w"); f.puts(String(app.port)); f.close();
setTimeout(() => app.close(), 40000);
`);
    // --std: the child imports "std". Without it the child died at once and this
    // whole suite reported itself SKIPPED (a pass) on every platform.
    helper = spawnHelper(`./dynajs --std ${P("srv.js")} >${P("srv.log")} 2>&1`);
    for (let i = 0; i < 60 && !port; i++) {
        sh("sleep 0.2");
        port = parseInt(out(`cat ${P("port")} 2>/dev/null`), 10) || 0;
    }
    if (!port) why = "the static server child did not start: " + out(`head -c 200 ${P("srv.log")} 2>/dev/null`).trim();
});
t.after(() => {
    if (helper) helper.stop();
    if (T) { sh(`pkill -f ${JSON.stringify(P("srv.js"))} 2>/dev/null`); removeAll(new Path(T)); }
});
const ready = (a) => {
    if (!why) return;
    if (REQUIRE) a.must(false, "REQUIRED: " + why);
    a.skip(why);
};

test("CONTROL: a legitimate file under the root IS served", (a) => {
    ready(a);
    ok(get("/s/ok.txt").indexOf("PUBLIC") >= 0,
       "CONTROL: a legitimate file under the root IS served");
});

const ESCAPES = [
    ["a plain ../ escape", "/s/../secret/creds.txt"],
    ["a percent-encoded ../", "/s/%2e%2e/secret/creds.txt"],
    ["a double percent-encoded ../", "/s/%252e%252e/secret/creds.txt"],
    ["a backslash variant", "/s/..\\secret\\creds.txt"],
    ["a SYMLINK to a directory outside the root", "/s/link/creds.txt"],
    ["a SYMLINK straight to the file", "/s/direct.txt"],
    ["an absolute path in the target", () => "/s//" + P("secret/creds.txt")],
];
for (const [what, rawTarget] of ESCAPES)
    test(what + " does not leak the file's CONTENT", (a) => {
        ready(a);
        const target = typeof rawTarget === "function" ? rawTarget() : rawTarget;
        ok(get(target).indexOf(CANARY) < 0,
           what + " does not leak the file's CONTENT");
    });

test("CONTROL: a nested file under the root is still served", (a) => {
    ready(a);
    sh(`mkdir -p ${P("www/sub")} && printf 'NESTED' > ${P("www/sub/deep.txt")}`);
    ok(get("/s/sub/deep.txt").indexOf("NESTED") >= 0,
       "CONTROL: a nested file under the root is still served");
});

await t.run();
