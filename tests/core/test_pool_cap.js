// flags: --std
// covers: c:src/dyna-aio.c
// The IO pool is one per process, sized to the usable CPU cores by default and at most (the fixed
// ceiling of 8 was lifted: CPU-bound 1 ms jobs ran 7.7k/s on 8 threads and 9.4k/s on all 10 cores
// of the bench machine); --io-threads takes 0..256 and clamps to the usable cores. A pool of n >= 1 threads
// runs one extra spare thread that takes cpu jobs only while all n hold blocking jobs (blocking jobs may use
// every thread: 10 us jobs on 2 threads ran 96k/s at the old n-1 limit, 188k/s now, libuv 192k/s).
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { args, Exec } from "dyna:sys";

const t = suite("pool_cap");
const DYN = args()[0];
const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_poolcap.${os.getpid()}.${Date.now()}`;
const sh = (cmd) => Exec("/bin/sh", ["-c", cmd], { timeoutMs: 10000, encoding: "utf8" });

// Usable cores: on Linux the affinity mask (nproc) further limited by a cgroup
// CPU quota, which is what a container with --cpus or --cpuset-cpus sees.
const USABLE = os.platform === "linux"
    ? 'n=$(nproc); q=$(cat /sys/fs/cgroup/cpu.max 2>/dev/null | awk \'$1 != "max" { print int(($1 + $2 - 1) / $2) }\'); '
        + 'if [ -n "$q" ] && [ "$q" -gt 0 ] && [ "$q" -lt "$n" ]; then n=$q; fi; echo $n'
    : "getconf _NPROCESSORS_ONLN";
const ncpu = parseInt(String(sh(USABLE).stdout).trim(), 10);
const CAP = Math.min(256, ncpu);
const POOL = (n) => n + 1;
// JS source text of a shell command counting the threads of the pid expression.
const COUNT = os.platform === "linux"
    ? (pid) => `"awk '/^Threads:/ { print $2 }' /proc/${pid}/status"`
    : (pid) => `"ps -M -p ${pid} | tail -n +2 | wc -l"`;

function write(path, content) {
    const f = std.open(path, "w");
    f.puts(content);
    f.close();
}

const SELF = [
    'import * as os from "os";',
    'import { App } from "dyna:http";',
    'import { Exec } from "dyna:sys";',
    'const app = scriptArgs.indexOf("bare") < 0 ? new App({ port: 0 }) : null;',
    'if (app && scriptArgs.indexOf("idle") < 0) app.start();',
    `const r = Exec("/bin/sh", ["-c", ${COUNT('" + os.getpid() + "')}], { encoding: "utf8" });`,
    'print("THREADS " + String(r.stdout).trim());',
    'if (app && scriptArgs.indexOf("idle") < 0) app.close();',
].join("\n");

const WORKER = [
    'import * as os from "os";',
    'import { App } from "dyna:http";',
    "const app = new App({ port: 0 });",
    "app.start();",
    "os.Worker.parent.postMessage(1);",
].join("\n");

const MAIN = [
    'import * as os from "os";',
    'import * as std from "std";',
    'import { Exec } from "dyna:sys";',
    "let seen = 0; const ws = [];",
    "for (let i = 0; i < 3; i++) {",
    `    const w = new os.Worker("${T}_w.js"); ws.push(w);`,
    "    w.onmessage = function () {",
    "        if (++seen < 3) return;",
    `        const r = Exec("/bin/sh", ["-c", ${COUNT('" + os.getpid() + "')}], { encoding: "utf8" });`,
    '        std.out.puts("THREADS " + String(r.stdout).trim() + "\\n"); std.out.flush();',
    "        std.exit(0);",
    "    };",
    "}",
].join("\n");

write(`${T}_self.js`, SELF);
write(`${T}_w.js`, WORKER);
write(`${T}_main.js`, MAIN);

function threads(flags, script, arg) {
    const r = sh(`'${DYN}' --std ${flags} ${T}_${script}.js ${arg || ""} 2>&1`);
    const m = String(r.stdout).match(/THREADS (\d+)/);
    return m ? parseInt(m[1], 10) : "no count: " + String(r.stdout).slice(0, 200);
}

// Threads of a process that never touches the pool: 1 natively, more under a
// CPU emulator, which adds its own. Every expectation below is relative to it.
const BASE = threads("", "self", "bare");

t.test("the machine reports a usable core count and a thread baseline", (a) => {
    a.ok(ncpu >= 1, "ncpu=" + ncpu);
    a.ok(typeof BASE === "number" && BASE >= 1, "baseline threads: " + BASE);
});

t.test("default pool is every usable core", (a) => {
    a.eq(threads("", "self"), BASE + POOL(CAP));
});

t.test("--io-threads lowers the pool", (a) => {
    a.eq(threads("--io-threads 1", "self"), BASE + POOL(1));
    if (CAP >= 3) a.eq(threads("--io-threads 3", "self"), BASE + POOL(3));
});

t.test("--io-threads 0 starts no pool", (a) => {
    a.eq(threads("--io-threads 0", "self"), BASE);
});

t.test("--io-threads above the core count clamps to it", (a) => {
    a.eq(threads("--io-threads " + (ncpu + 1), "self"), BASE + POOL(CAP));
    a.eq(threads("--io-threads 256", "self"), BASE + POOL(CAP));
});

t.test("--io-threads above 256 is refused", (a) => {
    for (const v of ["257", "1024", "99999"]) {
        const r = sh(`'${DYN}' --std --io-threads ${v} ${T}_self.js 2>&1; echo rc=$?`);
        a.ok(/must be 0\.\.256/.test(String(r.stdout)), v + ": " + String(r.stdout).slice(0, 80));
        a.ok(/rc=1/.test(String(r.stdout)), v + " exits 1");
    }
});

t.test("an App that is never started starts no pool and does not hold the process", (a) => {
    a.eq(threads("", "self", "idle"), BASE, "no pool threads, and the child exited without close()");
});

t.test("three Workers share the one pool", (a) => {
    a.eq(threads("", "main"), BASE + 3 + POOL(CAP));
});

t.test("cleanup", (a) => {
    for (const s of ["self", "w", "main"]) os.remove(`${T}_${s}.js`);
    a.ok(true);
});

await t.run();
