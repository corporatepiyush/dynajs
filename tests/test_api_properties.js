// flags: --std
import * as std from "std";
import * as os from "os";

const SCRATCH = (() => {
    const base = (std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "");
    const dir = `${base}/dynajs-props-${os.getpid ? os.getpid() : "x"}`;
    os.mkdir(dir, 0o700);
    const [prev, e] = os.getcwd();
    if (e || os.chdir(dir) !== 0)
        throw new Error(`refusing to sweep in ${prev}: cannot chdir to ${dir}`);
    return { dir, prev };
})();
function scratchCleanup() {
    os.chdir(SCRATCH.prev);
    const [names] = os.readdir(SCRATCH.dir);
    if (names) for (const n of names) if (n !== "." && n !== "..") os.remove(`${SCRATCH.dir}/${n}`);
    os.remove(SCRATCH.dir);
}

const MODULES = ["bytes", "cli", "compress", "config", "crypto", "csv",
    "dataframe", "decimal", "encoding", "file", "hash", "html", "log",
    "matcher", "mathx", "ml", "net", "random", "scrape", "semver",
    "serialize", "simd", "structures", "sys", "time", "url", "uuid",
    "validate", "xml", "yaml", "schema"];

const SKIP = {
    App: "binds a port", HTTPServer: "binds a port", HTTPServerAsync: "binds a port",
    TCPServer: "binds a port", UDPSocket: "binds a port", DNSServer: "binds a port",
    TCPProxy: "binds a port", HTTPClient: "network I/O", Fetcher: "network I/O",
    fetch: "network I/O",
    Crawl: "network I/O", DNSResolver: "network I/O", Redis: "connects out",
    PostgreSQL: "connects out", Watcher: "holds the loop open", Exec: "spawns",
    Which: "touches PATH", FileReader: "file handle", FileWriter: "file handle",
    File: "file handle", removeAll: "deletes", remove: "deletes",
    rename: "mutates the fs", move: "mutates the fs", symlink: "mutates the fs",
    chmod: "mutates the fs", makeDir: "mutates the fs", writeFile: "mutates the fs",
    chDir: "mutates the process",
    setEnv: "mutates the process", makeTempDir: "mutates the fs",
    makeTempFile: "mutates the fs", exit: "ends the process",
    now: "returns the clock", monotonic: "returns the clock",
    v4: "random by definition", v7: "time-ordered, not constant",
    ULID: "time-ordered", NanoID: "random", uuid: "random",
    randomBytes: "random", random: "random", shuffle: "random",
    hostname: "environment", pid: "environment", cwd: "environment",
    getEnv: "environment", environ: "environment", tempDir: "environment",
    memoryUsage: "environment", uptime: "environment", loadAvg: "environment",
    monotonicNano: "returns the clock", nowUnixNano: "returns the clock",
    nowNanos: "returns the clock", nowMillis: "returns the clock",
    cpuUsage: "returns the process meter", rusage: "returns the process meter",
    NanoIDAlphabet: "random",
    Random: "random", nextInt: "random", nextFloat: "random",
    Ed25519Generate: "generates a fresh keypair",
    X25519Generate: "generates a fresh keypair",
    "dyna:crypto.Bcrypt.hash": "generates a fresh random salt",
    "dyna:crypto.RSA.generate": "generates a fresh keypair",
    "dyna:crypto.ECDSA.generate": "generates a fresh keypair",
    "dyna:crypto.ECDH.generate": "generates a fresh keypair",
};

const PROBES = ["abc", "Hello, World", "a b\tc", "", "0", "123", "héllo",
                "x".repeat(64), "a,b,c", "{}", "[]"];
const NUMPROBES = [0, 1, -1, 2, 0.5, 100, 65535];

let pass = 0, fail = 0;
const fails = [];
const STRENGTH = { INVOLUTION: 0, ROUNDTRIP: 0, IDEMPOTENT: 0, TOTALORDER: 0,
                   BOOLEAN: 0, DETERMINISM: 0 };
const uncovered = [];
const structural = [];
const seen = new Set();

const ok = (c, w, d) => { if (c) pass++; else { fail++; fails.push(w + (d ? "  -- " + d : "")); } };
const same = (a, b) => {
    if (a === b) return true;
    if (typeof a === "number" && typeof b === "number")
        return Object.is(a, b) || Math.abs(a - b) < 1e-12;
    if (a && b && a.length !== undefined && b.length !== undefined
        && typeof a !== "string") {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (!same(a[i], b[i])) return false;
        return true;
    }
    try { return JSON.stringify(a) === JSON.stringify(b); } catch (e) { return false; }
};

const PAIR_SUFFIX = [["Encode", "Decode"], ["encode", "decode"],
                     ["Stringify", "Parse"], ["stringify", "parse"],
                     ["To", "From"], ["Marshal", "Unmarshal"]];

function tryCall(fn, host, args) {
    try { return { v: fn.apply(host, args) }; } catch (e) { return { e }; }
}

function firstAccepted(fn, host) {
    for (const p of PROBES) {
        const r = tryCall(fn, host, [p]);
        if (!r.e && r.v !== undefined) return [p];
    }
    for (const n of NUMPROBES) {
        const r = tryCall(fn, host, [n]);
        if (!r.e && r.v !== undefined) return [n];
    }
    return null;
}

function sweep(label, name, fn, host, ns) {
    if (seen.has(label)) return;
    seen.add(label);

    if (/^(is|has|can|should)[A-Z]/.test(name) || /^is[A-Z]/.test(name)) {
        const args = firstAccepted(fn, host);
        if (args) {
            const r = tryCall(fn, host, args);
            if (!r.e) {
                ok(typeof r.v === "boolean",
                   `${label} returns a boolean`, `got ${typeof r.v}`);
                STRENGTH.BOOLEAN++;
                return;
            }
        }
    }

    if (/^(compare|cmp)/i.test(name)) {
        for (const [a, b] of [["a", "b"], ["1", "2"], ["", "x"]]) {
            const ab = tryCall(fn, host, [a, b]), ba = tryCall(fn, host, [b, a]);
            const aa = tryCall(fn, host, [a, a]);
            if (ab.e || ba.e || aa.e) continue;
            if (typeof ab.v !== "number") continue;
            ok(Math.sign(ab.v) === -Math.sign(ba.v) && aa.v === 0,
               `${label} is an antisymmetric total order`,
               `cmp(a,b)=${ab.v} cmp(b,a)=${ba.v} cmp(a,a)=${aa.v}`);
            STRENGTH.TOTALORDER++;
            return;
        }
    }

    for (const [fwd, back] of PAIR_SUFFIX) {
        if (!name.endsWith(fwd)) continue;
        const other = name.slice(0, -fwd.length) + back;
        const inv = ns && ns[other];
        if (typeof inv !== "function") continue;
        let tested = false, bad = null;
        for (const p of PROBES) {
            const f = tryCall(fn, host, [p]);
            if (f.e || f.v === undefined) continue;
            const b = tryCall(inv, host, [f.v]);
            if (b.e || b.v === undefined) continue;
            let recovered = b.v;
            if (typeof recovered !== "string" && recovered &&
                typeof recovered.length === "number" &&
                typeof recovered[0] === "number") {
                let t = "";
                for (let i = 0; i < recovered.length; i++)
                    t += String.fromCharCode(recovered[i]);
                recovered = t;
            }
            if (typeof recovered !== typeof p) { structural.push(`${label} + ${other}`); tested = false; break; }
            tested = true;
            if (!same(recovered, p)) { bad = `${JSON.stringify(p.slice(0, 16))} -> ${JSON.stringify(String(recovered).slice(0, 16))}`; break; }
        }
        if (tested) {
            ok(!bad, `${label} + ${other} round trip`, bad);
            STRENGTH.ROUNDTRIP++;
            return;
        }
    }

    const args = firstAccepted(fn, host);
    if (!args) { uncovered.push(`${label}: no probe accepted`); return; }
    const first = tryCall(fn, host, args);
    if (first.e || first.v === undefined) { uncovered.push(`${label}: no value`); return; }

    if (typeof first.v === typeof args[0]) {
        const twice = tryCall(fn, host, [first.v]);
        if (!twice.e && twice.v !== undefined) {
            if (same(twice.v, args[0]) && !same(first.v, args[0])) {
                ok(true, `${label} is an involution`);
                STRENGTH.INVOLUTION++;
                return;
            }
            if (same(twice.v, first.v)) {
                ok(true, `${label} is idempotent`);
                STRENGTH.IDEMPOTENT++;
                return;
            }
        }
    }

    const again = tryCall(fn, host, args);
    ok(!again.e && same(again.v, first.v),
       `${label} is deterministic`,
       again.e ? `second call threw ${again.e.message}`
               : `${JSON.stringify(String(first.v).slice(0, 24))} then ${JSON.stringify(String(again.v).slice(0, 24))}`);
    STRENGTH.DETERMINISM++;
}

for (const mod of MODULES) {
    let ns;
    try { ns = await import("dyna:" + mod); } catch (e) { continue; }
    for (const key of Object.getOwnPropertyNames(ns)) {
        if (key === "default" || key === "__esModule" || SKIP[key]) continue;
        let v;
        try { v = ns[key]; } catch (e) { continue; }
        if (typeof v === "function") {
            const isClass = v.prototype &&
                  Object.getOwnPropertyNames(v.prototype).length > 1;
            const label = `dyna:${mod}.${key}`;
            if (!isClass && !SKIP[label]) sweep(label, key, v, ns, ns);
            continue;
        }
        if (v && typeof v === "object") {
            for (const sub of Object.getOwnPropertyNames(v)) {
                if (SKIP[sub]) continue;
                let f;
                try { f = v[sub]; } catch (e) { continue; }
                const label = `dyna:${mod}.${key}.${sub}`;
                if (typeof f === "function" && !SKIP[label])
                    sweep(label, sub, f, v, v);
            }
        }
    }
}

print("\n" + "=".repeat(66));
if (fails.length) {
    print(`FAILURES (${fails.length}):`);
    for (const f of fails.slice(0, 30)) print("  " + f);
    if (fails.length > 30) print(`  ... and ${fails.length - 30} more`);
}
print("assertion strength -- these are NOT interchangeable:");
for (const k of ["INVOLUTION", "ROUNDTRIP", "IDEMPOTENT", "TOTALORDER",
                 "BOOLEAN", "DETERMINISM"])
    print(`  ${k.padEnd(12)} ${String(STRENGTH[k]).padStart(4)}`);
const real = STRENGTH.INVOLUTION + STRENGTH.ROUNDTRIP + STRENGTH.IDEMPOTENT +
             STRENGTH.TOTALORDER + STRENGTH.BOOLEAN;
print(`  ${"(no property)".padEnd(12)} ${String(uncovered.length).padStart(4)}` +
      "  -- listed, NOT counted as covered");
if (structural.length)
    print(`  ${"(structural)".padEnd(12)} ${String(structural.length).padStart(4)}` +
          "  -- pair returns a node/record, not a comparable value");
print(`\n${real} names carry a property that pins something real; ` +
      `${STRENGTH.DETERMINISM} carry determinism only.`);
print("Determinism is NOT coverage. The hand-written suites remain the only");
print("layer that says an answer is RIGHT.");
scratchCleanup();
print(`test_api_properties: ${pass} passed, ${fail} failed`);
if (fail > 0) std.exit(1);
