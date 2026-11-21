// covers: dyna:sys.Spawn
// F31: an uncaught error while a Spawn had a parked stdout/stderr read() aborted the process
// ("Assertion failed: list_empty(&rt->gc_obj_list)" in JS_FreeRuntime, SIGABRT) instead of exiting 1.
// A parked read keeps its buffer, resolve/reject functions and promise in the native Spawn struct, outside
// the GC's view; only wait() had a shutdown sweep, so on the error path (the loop stops with the read still
// parked) nothing released them and teardown found live objects. Every parked operation now registers the
// Spawn's one shutdown sweep: it rejects a parked read(), flush() and wait() while a context exists, and
// only frees them when none does. Each fixture runs in a child dynajs and must exit with code 1 (no
// signal), print the script's own error, and print no assertion; the first fixture is the original shape
// (several Spawns, Promise.all of drains, the throw after wait()).
import { suite } from "../lib/kit.js";
import { args, Spawn } from "dyna:sys";
import * as file from "dyna:file";
import { Path } from "dyna:file";

const t = suite("test_sys_spawn_shutdown_pending");
const BIN = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];
const DIR = file.makeTempDir("spawnpend_");

const HEAD = `import * as sys from "dyna:sys";
async function drain(pipe) { const b = new Uint8Array(65536); for (;;) { if (await pipe.read(b) <= 0) break; } }
`;
const FIXTURES = {
    "original shape: Promise.all of drains, throw after wait()": `
async function one(i) {
    const c = new sys.Spawn("sh", ["-c", i ? "sleep 3" : "echo hi"], { stdin: "ignore", timeoutMs: 3000 });
    await Promise.all([drain(c.stdout), drain(c.stderr)]);
    await c.wait();
    c.close();
    undefined.code;
}
await Promise.all([0, 1, 2, 3].map(one));`,
    "parked stdout read, top-level throw": `
const c = new sys.Spawn("sleep", ["3"], { stdin: "ignore" });
drain(c.stdout);
throw new TypeError("fixture boom");`,
    "parked reads on both pipes, async throw": `
const c = new sys.Spawn("sleep", ["3"], { stdin: "ignore" });
drain(c.stdout); drain(c.stderr);
await (async () => { await null; null.x; })();`,
    "parked stdin flush, throw": `
const c = new sys.Spawn("sleep", ["3"], { stdin: "pipe" });
c.stdin.write(new Uint8Array(4 << 20));
const f = c.stdin.flush();
await null;
throw new TypeError("fixture boom");`,
    "parked read + wait + flush together, throw": `
const c = new sys.Spawn("sleep", ["3"], { stdin: "pipe" });
drain(c.stdout); c.wait();
c.stdin.write(new Uint8Array(4 << 20)); c.stdin.flush();
await null;
throw new TypeError("fixture boom");`,
};

async function readAll(pipe) {
    const parts = [], b = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(b); if (n <= 0) break; parts.push(new TextDecoder().decode(b.subarray(0, n))); }
    return parts.join("");
}

let k = 0;
for (const [name, body] of Object.entries(FIXTURES)) {
    const path = DIR.join("fx" + (k++) + ".js");
    file.writeFile(path, HEAD + body + "\n");
    t.test(name, async (a) => {
        const p = new Spawn(BIN, ["--std", String(path)], { stdin: "ignore", timeoutMs: 15000 });
        const [out, err] = await Promise.all([readAll(p.stdout), readAll(p.stderr)]);
        const r = await p.wait();
        p.close();
        const all = out + err;
        a.eq(r.timedOut, false, "finished in time");
        a.eq(r.signal, null, "not killed by a signal (F31 was SIGABRT)");
        a.eq(r.code, 1, "uncaught error exit code");
        a.ok(/TypeError/.test(all), "the script's own error is printed");
        a.ok(!/Assertion failed|gc_obj_list/.test(all), "no teardown assertion: " + JSON.stringify(all.slice(0, 300)));
    }, { timeoutMs: 20000 });
}

t.after(() => file.removeAll(DIR));
await t.run();
