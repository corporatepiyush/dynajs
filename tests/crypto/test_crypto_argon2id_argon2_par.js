// flags: --std
// covers: dyna:crypto.Argon2id
// Argon2id lanes run on dyn-par workers inside one hash call (one barrier per slice, RFC 9106 3.4:
// a segment reads other lanes only outside its own slice). Oracles:
//  1. external vectors from OpenSSL 3.5's ARGON2ID KDF (tests/crypto/gen_argon2_par_vectors.sh; that
//     implementation reproduces RFC 9106 section 5.3's tag 0d640df5...; our API has no secret/AD, so the
//     RFC vector itself cannot be fed), p = 1..16, both sides of the parallel crossover;
//  2. a serial-vs-parallel differential over random (t, m, p in 1..16): the same list hashed here, in a
//     child with --io-threads 0 (serial), --io-threads 3, and children whose helper spawns fail after
//     0/1/2 threads (the test hook DYNAJS_PAR_SPAWN_LIMIT, which accepts only 0..64 written as plain
//     digits: anything else is ignored with one stderr line, checked here);
//  3. sys.parStats proves the parallel path ran (helpers spawned) and the serial child spawned none;
//  4. four os.Workers hashing at once never hold more helpers than the budget, and agree.
import * as os from "os";
import * as std from "std";
import { Argon2id } from "dyna:crypto";
import { args, Exec, parStats } from "dyna:sys";
import { suite } from "../lib/kit.js";

const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const enc = (s) => new TextEncoder().encode(s);
const h = async (pw, salt, t, m, p) => hex(await Argon2id.hash(pw, enc(salt), { iterations: t, memory: m, parallelism: p, hashLen: 32, encoded: false }));

const VECTORS = [
    [1, 128, 16, "pw-0-par", "saltsalt-0-xyz", "71d8070a5043fd45512ee44a5feac7b5ba0e43fbbd9ce2fefd6f2f106bd3dc32"],
    [3, 32, 4, "pw-1-par", "saltsalt-1-xyz", "f4e5c290bd825a4c7214db61cec19f1d57113c5c0e73468673a925f723b52cb0"],
    [2, 64, 1, "pw-2-par", "saltsalt-2-xyz", "bfac843ab75487a905d6e7e48275b61d5b2c4ddb2e8ac92ed1e7f4979cf9fe1f"],
    [1, 1024, 4, "pw-3-par", "saltsalt-3-xyz", "56d3cc50e7f288225d17542cbe832ddc9390f3456b48e4a5b7b62f4ea6672549"],
    [3, 4096, 4, "pw-4-par", "saltsalt-4-xyz", "d0ad0c30ba0a714be715dcc5cb4a93024cd461f3876e60b4fabba6277cc13f07"],
    [2, 4096, 3, "pw-5-par", "saltsalt-5-xyz", "75e6875ea41334308309ebbaad8fcafc5ae1ae21c5d92a730b6757cd14e7b1bc"],
    [1, 2048, 8, "pw-6-par", "saltsalt-6-xyz", "0cc2ab879cd8037b6518557add526807c8579a292111308c33f28cf56ad8721f"],
    [2, 16384, 16, "pw-7-par", "saltsalt-7-xyz", "899d83765b50fff6d244260da4aefd9fc12c76d85642a867024aff27f9309b13"],
    [1, 65536, 4, "pw-8-par", "saltsalt-8-xyz", "dedf335d0adde9224a7d600c782c75f4b25d4a9129d969d17d89f9e3db5e7e5f"],
    [3, 8192, 5, "pw-9-par", "saltsalt-9-xyz", "5104a52395481ff7debe5d45523454a0f78b23ed7d2b3f737cf9ffe3a281f445"],
    [1, 512, 2, "pw-10-par", "saltsalt-10-xyz", "2cae8b7877ed886a2e1fc73cf88d2f11477cc60fb48cfa5d3c2d34404e5c2d75"],
    [4, 1000, 7, "pw-11-par", "saltsalt-11-xyz", "1259815cd2cb29bac354328b2cda2a62068d1f48778054a737b1b705ffca8e96"],
    [1, 4096, 16, "pw-12-par", "saltsalt-12-xyz", "075a31f959ef7e96f3139fc008c0f1269833e7d96f17546016d7056b6c99ab03"],
    [2, 2048, 1, "pw-13-par", "saltsalt-13-xyz", "8ceefbddd9b0fae2d79622bc338dc42aa3e1ee1860feda7affd068d5e0491ba9"],
    [1, 3000, 6, "pw-14-par", "saltsalt-14-xyz", "c21cfdc9dda17c0ef03376041becbb55ec0f53231029407e8b537ced1792b2bf"],
];

function diffCases() {
    let s = 0x9e3779b9;
    const rnd = (n) => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return (s >>> 8) % n; };
    const out = [];
    for (let i = 0; i < 28; i++) {
        const p = 1 + rnd(16), t = 1 + rnd(3);
        const m = 8 * p + rnd(i % 3 === 0 ? 64 * p : 1200 * p);
        out.push([t, m, p, "diff-pw-" + i, "diff-salt-" + i]);
    }
    out.push([2, 4096, 16, "p16", "p16-salt-x"], [1, 8192, 13, "p13", "p13-salt-x"], [3, 2048, 2, "p2", "p2-salt-xx"]);
    return out;
}
const digests = async () => { const o = []; for (const c of diffCases()) o.push(await h(c[3], c[4], c[0], c[1], c[2])); return o; };

if (scriptArgs[1] === "--child") {
    const before = parStats();
    const d = await digests();
    const after = parStats();
    print(JSON.stringify({ d, spawned: after.spawned - before.spawned, failures: after.spawnFailures - before.spawnFailures }));
    std.exit(0);
}

const BIN = args()[0];
const SELF = scriptArgs[0];
function child(flags, env) {
    const r = Exec(BIN, [...flags, "--std", SELF, "--child"], { timeoutMs: 120000, encoding: "utf8", env: env || {} });
    if (r.code !== 0) throw new Error("child " + flags.join(" ") + " exited " + r.code + ": " + String(r.stderr).slice(0, 300));
    const out = JSON.parse(String(r.stdout).trim().split("\n").pop());
    out.stderr = String(r.stderr || "");
    return out;
}

const t = suite("argon2-par");

t.test("OpenSSL ARGON2ID vectors, p = 1..16, both sides of the crossover", async (a) => {
    for (const [tt, m, p, pw, salt, tag] of VECTORS) a.eq(await h(pw, salt, tt, m, p), tag, `t=${tt} m=${m} p=${p}`);
});

t.test("the parallel path runs: a big p=4 hash spawns helpers", async (a) => {
    const before = parStats();
    await h("pw", "saltsalt", 1, 8192, 4);
    const after = parStats();
    if (after.budget === 0) a.skip("single-CPU host: no helper budget");
    a.ok(after.spawned > before.spawned, "helpers spawned " + (after.spawned - before.spawned));
    a.eq(after.live, 0, "no helper outlives the call");
});

t.test("below the crossover stays serial (m = 8p blocks, p = 4)", async (a) => {
    const before = parStats();
    await h("pw", "saltsalt", 3, 32, 4);
    a.eq(parStats().spawned, before.spawned, "no helpers for 2-block segments");
});

t.test("serial vs parallel differential over random (t, m, p)", async (a) => {
    const here = await digests();
    const serial = child(["--io-threads", "0"]);
    a.eq(serial.spawned, 0, "--io-threads 0 spawns no helper");
    a.deep(serial.d, here, "--io-threads 0 (serial) equals the parallel run");
    const three = child(["--io-threads", "3"]);
    a.deep(three.d, here, "--io-threads 3 equals the parallel run");
    for (const lim of ["0", "1", "2"]) {
        const c = child([], { DYNAJS_PAR_SPAWN_LIMIT: lim });
        a.deep(c.d, here, "spawn limit " + lim + " equals the parallel run");
        if (parStats().budget > Number(lim)) a.ok(c.failures > 0, "spawn limit " + lim + " injected failures (" + c.failures + ")");
    }
});

t.test("DYNAJS_PAR_SPAWN_LIMIT is parsed strictly: malformed values are ignored with one warning", async (a) => {
    const here = await digests();
    for (const bad of ["abc", "-1", "3x", " 2", "99", "0064", "1e1", "+1"]) {
        const c = child([], { DYNAJS_PAR_SPAWN_LIMIT: bad });
        a.deep(c.d, here, "limit " + JSON.stringify(bad) + " leaves the answer unchanged");
        a.eq((c.stderr.match(/DYNAJS_PAR_SPAWN_LIMIT=.*ignored/g) || []).length, 1, "limit " + JSON.stringify(bad) + " warns exactly once");
        a.eq(c.failures, 0, "limit " + JSON.stringify(bad) + " is not applied");
        if (parStats().budget > 0) a.ok(c.spawned > 0, "limit " + JSON.stringify(bad) + " leaves helpers on");
    }
    const ok = child([], { DYNAJS_PAR_SPAWN_LIMIT: "1" });
    a.eq(ok.stderr.indexOf("ignored"), -1, "a well-formed limit prints nothing");
}, { timeoutMs: 120000 });

t.test("four os.Workers hashing at once stay within the helper budget and agree", async (a) => {
    const want = await h("wk", "worker-salt", 2, 16384, 8);
    const tmp = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "");
    const path = `${tmp}/dynajs_argon2_par_${os.getpid()}.js`;
    const f = std.open(path, "w");
    f.puts(`import * as os from "os"; import { Argon2id } from "dyna:crypto";
const p = os.Worker.parent;
p.onmessage = async () => { p.onmessage = null;
  const u = await Argon2id.hash("wk", new TextEncoder().encode("worker-salt"), { iterations: 2, memory: 16384, parallelism: 8, hashLen: 32, encoded: false });
  p.postMessage(Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("")); };`);
    f.close();
    const got = [];
    let peak = 0, budget = parStats().budget;
    await new Promise((resolve) => {
        const ws = [];
        for (let i = 0; i < 4; i++) {
            const w = new os.Worker(path);
            w.onmessage = (e) => { got.push(e.data); w.onmessage = null; if (got.length === 4) resolve(); };
            ws.push(w);
        }
        for (const w of ws) w.postMessage("go");
        const poll = () => { const s = parStats(); peak = Math.max(peak, s.live); if (got.length < 4) setTimeout(poll, 1); };
        poll();
    });
    os.remove(path);
    const s = parStats();
    a.deep(got, [want, want, want, want], "every worker computed the same tag");
    a.ok(peak <= budget && s.peak <= budget, "live helpers peaked at " + Math.max(peak, s.peak) + " of budget " + budget);
    a.eq(s.live, 0, "no helper outlives the calls");
}, { timeoutMs: 60000 });

await t.run();
