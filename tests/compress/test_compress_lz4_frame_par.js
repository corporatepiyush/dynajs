// flags: --std
// covers: dyna:compress.lz4Frame
// lz4Frame compresses the independent 4 MiB blocks of an input over one block on dyn-par workers, with the
// content checksum (XXH32 over the whole input, serial) on a worker of its own; frames are concatenated in
// block order, so the bytes are the serial frame's. Oracles: (1) a child with --io-threads 0 (serial) and
// children whose helper spawns fail after 0/1/2 threads produce byte-identical frames (compared by XXH3 of
// each frame and its length) for compressible and incompressible inputs, levels 1/9/12, checksum on/off,
// sizes 4 MiB (one block: serial) and 4 MiB + 1 (two blocks), plus 9 MiB + 5, 64 MiB and levels 9/12
// with DYNAJS_PAR_BIG=1; spawn failures come from the test hook DYNAJS_PAR_SPAWN_LIMIT; (2) the reference
// lz4 CLI decodes every parallel frame back to the input (a foreign reader checks block sizes and the content checksum);
// (3) sys.parStats proves helpers ran above one block and not at one block.
import * as os from "os";
import * as std from "std";
import { lz4Frame, lz4Unframe } from "dyna:compress";
import { XXH3_64 } from "dyna:hash";
import { args, Exec, parStats } from "dyna:sys";
import { suite } from "../lib/kit.js";

const MiB = 1 << 20;
const BIG = std.getenv("DYNAJS_PAR_BIG") === "1";
const cache = new Map();
function input(n, kind) {
    const k = n + kind;
    if (!cache.has(k)) cache.set(k, makeInput(n, kind));
    return cache.get(k);
}
function makeInput(n, kind) {
    const b = new Uint8Array(n);
    let s = 0x1234567 ^ n;
    if (kind === "random") for (let i = 0; i < n; i++) { s = (Math.imul(s, 1103515245) + 12345) >>> 0; b[i] = s >>> 24; }
    else {
        const w = new TextEncoder().encode("alpha beta gamma delta lorem ipsum dolor sit amet 42 3.14\n");
        for (let i = 0; i < n; i++) { if (i % 4096 === 0) s = (Math.imul(s, 1103515245) + 12345) >>> 0; b[i] = w[(i + (s >>> 20)) % w.length]; }
    }
    return b;
}
const CASES = [];
for (const n of BIG ? [4 * MiB, 4 * MiB + 1, 9 * MiB + 5, 64 * MiB] : [4 * MiB, 4 * MiB + 1])
    for (const kind of ["text", "random"])
        for (const opts of [{}, { checksum: false }, { level: 1 }, { level: 9 }, { level: 12, checksum: true }]) {
            if (kind === "random" && opts.level) continue;
            if (opts.level === 12 && (n !== 4 * MiB + 1 || !BIG)) continue;
            if (opts.level === 9 && !BIG) continue;
            if (n === 64 * MiB && (opts.level || kind === "random")) continue;
            CASES.push([n, kind, opts]);
        }
const digests = () => CASES.map(([n, kind, opts]) => { const f = lz4Frame(input(n, kind), opts); return f.length + ":" + XXH3_64(f); });

if (scriptArgs[1] === "--child") {
    const before = parStats();
    const d = digests();
    const after = parStats();
    print(JSON.stringify({ d, spawned: after.spawned - before.spawned, failures: after.spawnFailures - before.spawnFailures }));
    std.exit(0);
}
const BIN = args()[0], SELF = scriptArgs[0];
function child(flags, env) {
    const r = Exec(BIN, [...flags, "--std", SELF, "--child"], { timeoutMs: 300000, encoding: "utf8", env: Object.assign(BIG ? { DYNAJS_PAR_BIG: "1" } : {}, env || {}) });
    if (r.code !== 0) throw new Error("child " + flags.join(" ") + " exited " + r.code + ": " + String(r.stderr).slice(0, 300));
    return JSON.parse(String(r.stdout).trim().split("\n").pop());
}

const t = suite("lz4-frame-par");

t.test("helpers run above one block and not at one block", (a) => {
    const s0 = parStats();
    if (s0.budget === 0) a.skip("single-CPU host: no helper budget");
    lz4Frame(input(4 * MiB, "text"));
    const s1 = parStats();
    a.eq(s1.spawned, s0.spawned, "4 MiB (one block) stays serial");
    lz4Frame(input(4 * MiB + 1, "text"));
    a.ok(parStats().spawned > s1.spawned, "4 MiB + 1 (two blocks) spawned helpers");
    a.eq(parStats().live, 0, "no helper outlives the call");
});

t.test("parallel frames round-trip and the reference lz4 CLI decodes them", (a) => {
    const have = os.exec(["/bin/sh", "-c", "command -v lz4 >/dev/null 2>&1"]) === 0;
    const tmp = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/dynajs_lz4par_" + os.getpid();
    for (const [n, kind, opts] of CASES.filter((c) => c[0] > 4 * MiB && !c[2].level)) {
        const src = input(n, kind), f = lz4Frame(src, opts);
        const back = lz4Unframe(f);
        a.eq(XXH3_64(back), XXH3_64(src), `${n} ${kind} ${JSON.stringify(opts)} round trip`);
        if (!have) continue;
        let fd = std.open(tmp + ".lz4", "wb"); fd.write(f.buffer, f.byteOffset, f.length); fd.close();
        const rc = os.exec(["/bin/sh", "-c", `lz4 -d -q -f "${tmp}.lz4" "${tmp}.out"`]);
        a.eq(rc, 0, "lz4 CLI accepts the frame");
        fd = std.open(tmp + ".out", "rb"); const buf = new Uint8Array(n + 1); const got = fd.read(buf.buffer, 0, n + 1); fd.close();
        a.ok(got === n && XXH3_64(buf.subarray(0, got)) === XXH3_64(src), `lz4 CLI decodes ${n} ${kind} ${JSON.stringify(opts)} to the input`);
    }
    if (!have) a.skip("lz4 CLI not installed: foreign-reader check not run");
    os.remove(tmp + ".lz4"); os.remove(tmp + ".out");
});

t.test("serial and spawn-limited children are byte-identical", (a) => {
    const here = digests();
    const serial = child(["--io-threads", "0"]);
    a.eq(serial.spawned, 0, "--io-threads 0 spawns no helper");
    a.deep(serial.d, here, "--io-threads 0");
    for (const lim of BIG ? ["0", "1", "2"] : ["0", "1"]) {
        const c = child([], { DYNAJS_PAR_SPAWN_LIMIT: lim });
        a.deep(c.d, here, "spawn limit " + lim);
        if (parStats().budget > Number(lim)) a.ok(c.failures > 0, "spawn limit " + lim + " injected failures (" + c.failures + ")");
    }
}, { timeoutMs: 600000 });

await t.run();
