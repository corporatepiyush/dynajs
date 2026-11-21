// flags: --std
// covers: contract dyna:bytes.* dyna:cli.* dyna:compress.* dyna:config.* dyna:crypto.* dyna:csv.* dyna:dataframe.* dyna:decimal.* dyna:dns.* dyna:encoding.* dyna:file.* dyna:hash.* dyna:html.* dyna:http.* dyna:log.* dyna:matcher.* dyna:metrics.* dyna:mathx.* dyna:ml.* dyna:net.* dyna:netip.* dyna:proxy.* dyna:random.* dyna:scrape.* dyna:semver.* dyna:serialize.* dyna:simd.* dyna:structures.* dyna:sys.* dyna:time.* dyna:url.* dyna:uuid.* dyna:validate.* dyna:xml.* dyna:yaml.* -- random-argument fuzz over every module's exported names
// Round trips: no input may throw, the dyna:encoding text equals tests/lib/codec_model.js and gzip
// output carries the 1f8b08 header and an independently computed CRC-32/ISIZE trailer. The old
// loop skipped throws and string inputs whose decode returned bytes, so half the inputs were
// never checked; it also hid that fuzzing dyna:sys.setNativeMemoryLimit left a random cap in
// force (seeds 1, 2, 5: every later call and round trip saw only out-of-memory). The sweep now
// lifts the cap after each such call and asserts none is left. Mutants: BX_B58 "21345..." (red);
// dropping the cap reset (seed 1 red).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_api_fuzz");
import * as std from "std";
import * as os from "os";

// The cwd stays in the scratch dir to the end and the dir is removed under it: an async fs call the sweep
// started resolves its relative path after the script returns, so it must fail there, never land in the caller's cwd.
const SCRATCH = (() => {
    const base = (std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "");
    const dir = `${base}/dynajs-fuzz-${os.getpid ? os.getpid() : "x"}`;
    os.mkdir(dir, 0o700);
    const [prev, e] = os.getcwd();
    if (e || os.chdir(dir) !== 0)
        throw new Error(`refusing to fuzz in ${prev}: cannot chdir to ${dir}`);
    return { dir, prev };
})();
function scratchCleanup() {
    const [names] = os.readdir(SCRATCH.dir);
    if (names) for (const n of names) if (n !== "." && n !== "..") os.remove(`${SCRATCH.dir}/${n}`);
    os.remove(SCRATCH.dir);
}

import { STRINGS, PRNG, generator, shrink, show } from "../lib/fuzzgen.js";
import { mark, spent } from "../lib/cpuclock.js";
import { setNativeMemoryLimit, memoryUsage } from "dyna:sys";
import { ENCODERS, bytesOf, sameBytes, crc32 } from "../lib/codec_model.js";
const u32le = (b, i) => (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0;
const gzipFramed = (z, raw) => z.length >= 18 && z[0] === 0x1f && z[1] === 0x8b && z[2] === 8 &&
    u32le(z, z.length - 8) === crc32(raw) && u32le(z, z.length - 4) === raw.length >>> 0;

const lone = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:[^\uD800-\uDBFF]|^)[\uDC00-\uDFFF]/;
const ENCODABLE = STRINGS.filter(s => !lone.test(s));
const SEED = parseInt(std.getenv("DYNA_FUZZ_SEED") || "1337", 10);
const ROUNDS = parseInt(std.getenv("DYNA_FUZZ_ROUNDS") || "40", 10);
// The wall-clock bound is overridable so a SANITIZER leg (which is
// several times slower) does not assert a release-build duration against
// itself; the engine-side fix (work-based bounds) is B1-15/TEST-03.
const BUDGET_MS = +(std.getenv("DYNAJS_BUDGET_MS") || 1500);
const rnd = PRNG(SEED);
const G = generator(rnd);
const argsFor = () => G.args(3);
const showArgs = show;

const MODULES = ["bytes", "cli", "compress", "config", "crypto", "csv", "dns", "http", "metrics", "netip", "proxy",
    "dataframe", "decimal", "encoding", "file", "hash", "html", "log",
    "matcher", "mathx", "ml", "net", "random", "scrape", "semver",
    "serialize", "simd", "structures", "sys", "time", "url", "uuid",
    "validate", "xml", "yaml"];

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
    makeTempFile: "mutates the fs",
    open: "creates the file it names", writer: "creates the file it names", reader: "file handle",
    copyFile: "mutates the fs", FileLock: "creates a lock file",
};

const ROUND_TRIPS = [
    ["encoding", "HexEncode", "HexDecode"],
    ["encoding", "Base64Encode", "Base64Decode"],
    ["encoding", "Base64URLEncode", "Base64URLDecode"],
    ["encoding", "Base32Encode", "Base32Decode"],
    ["encoding", "Base32HexEncode", "Base32HexDecode"],
    ["encoding", "Base58Encode", "Base58Decode"],
    ["encoding", "Base85Encode", "Base85Decode"],
    ["serialize", "CBOREncode", "CBORDecode"],
    ["serialize", "MsgPackEncode", "MsgPackDecode"],
    ["compress", "gzip", "gunzip"],
    ["compress", "lz4Frame", "lz4Unframe"],
];

const DETERMINISTIC = /^(SHA|MD5|BLAKE|Keccak|CRC|Murmur|Hex|Base|HMAC|Stable|compare|satisfies|major|minor|patch|isValid|erf|gcd|Levenshtein|Dice)/;

let pass = 0, fail = 0, skipped = 0, calls = 0;
const fails = [], skips = [];

function record(label, why) { fail++; fails.push(`${label}: ${why}`); }

function fuzzOne(label, fn, ctor, host, deterministic) {
    for (let round = 0; round < ROUNDS; round++) {
        const a = argsFor();
        let got, threw = null;
        const t0 = mark();
        try { got = ctor ? new fn(...a) : fn.apply(host, a); }
        catch (e) { threw = e; }
        const dt = spent(t0);
        if (fn === setNativeMemoryLimit) setNativeMemoryLimit(0);
        calls++;

        if (dt >= BUDGET_MS) {
            const still = (t) => {
                const s0 = mark();
                try { ctor ? new fn(...t) : fn.apply(host, t); } catch (e) {}
                return spent(s0) >= BUDGET_MS;
            };
            const min = shrink(a, still, 40);
            record(label, `took ${dt.toFixed(0)}ms; minimal input ${showArgs(min)} (seed ${SEED})`);
            return;
        }
        if (threw && typeof threw.message !== "string" && threw instanceof Error) {
            record(label, `threw an Error with a non-string message on ${showArgs(a)}`);
            return;
        }
        if (deterministic && !threw) {
            let again, threw2 = null;
            try { again = ctor ? new fn(...a) : fn.apply(host, a); }
            catch (e) { threw2 = e; }
            const same = threw2 ? false
                : (typeof got === "object" ? String(got) === String(again) : Object.is(got, again));
            if (!same && got === got  ) {
                const still = (t) => {
                    let x, y;
                    try { x = ctor ? new fn(...t) : fn.apply(host, t); } catch (e) { return false; }
                    try { y = ctor ? new fn(...t) : fn.apply(host, t); } catch (e) { return true; }
                    return !(typeof x === "object" ? String(x) === String(y) : Object.is(x, y));
                };
                const min = shrink(a, still, 200);
                record(label, `not deterministic; minimal input ${showArgs(min)} (seed ${SEED})`);
                return;
            }
        }
        if (got && (typeof got === "object" || typeof got === "function")) {
            try { if (typeof got.cancel === "function") got.cancel(); } catch (e) {}
            try { if (typeof got.close === "function") got.close(); } catch (e) {}
            try { if (typeof got.then === "function" && typeof got.catch === "function")
                got.catch(function () {}); } catch (e) {}
        }
    }
    pass++;
}

print(`test_api_fuzz: seed ${SEED}, ${ROUNDS} rounds per name`);
print("re-run a failure with DYNA_FUZZ_SEED=<seed> for a byte-identical replay\n");

for (const name of MODULES) {
    let ns;
    try { ns = await import("dyna:" + name); }
    catch (e) { skipped++; skips.push(`dyna:${name}: not in this build`); continue; }
    let n = 0;
    for (const key of Object.getOwnPropertyNames(ns)) {
        if (key === "default" || key === "__esModule") continue;
        if (SKIP[key]) { skipped++; skips.push(`dyna:${name}.${key}: ${SKIP[key]}`); continue; }
        let v;
        try { v = ns[key]; } catch (e) { continue; }
        const det = DETERMINISTIC.test(key);

        if (typeof v === "function") {
            const isClass = v.prototype &&
                  Object.getOwnPropertyNames(v.prototype).length > 1;
            fuzzOne(`dyna:${name}.${key}`, v, isClass, ns, det && !isClass);
            n++;
            if (isClass) {
                let inst = null;
                for (let i = 0; i < 24 && !inst; i++) {
                    try { inst = new v(...argsFor()); } catch (e) {}
                }
                if (!inst) continue;
                for (const mn of Object.getOwnPropertyNames(v.prototype)) {
                    if (mn === "constructor" || SKIP[mn]) continue;
                    let mv;
                    try { mv = v.prototype[mn]; } catch (e) { continue; }
                    if (typeof mv !== "function") continue;
                    fuzzOne(`dyna:${name}.${key}#${mn}`, mv, false, inst, false);
                    n++;
                }
            }
        } else if (v && typeof v === "object") {
            for (const sub of Object.getOwnPropertyNames(v)) {
                if (typeof v[sub] !== "function") continue;
                if (SKIP[sub]) { skipped++; skips.push(`dyna:${name}.${key}.${sub}: ${SKIP[sub]}`); continue; }
                fuzzOne(`dyna:${name}.${key}.${sub}`, v[sub], false, v,
                        DETERMINISTIC.test(sub));
                n++;
            }
        }
    }
    print(`  ${("dyna:" + name).padEnd(18)} ${String(n).padStart(4)} names fuzzed`);
}

print("\n-- round trips: decode(encode(x)) must reproduce x --");
if (memoryUsage().nativeLimit === 0) pass++;
else record("dyna:sys.setNativeMemoryLimit", `the sweep left a ${memoryUsage().nativeLimit}-byte native cap, so every later call would only see out-of-memory`);
for (const [modName, encName, decName] of ROUND_TRIPS) {
    let m;
    try { m = await import("dyna:" + modName); } catch (e) { continue; }
    const enc = m[encName], dec = m[decName];
    if (typeof enc !== "function" || typeof dec !== "function") {
        skipped++; skips.push(`${modName}.${encName}/${decName}: absent`); continue;
    }
    let bad = null;
    const model = ENCODERS[encName], keepsStrings = modName === "serialize";
    for (let round = 0; round < ROUNDS && !bad; round++) {
        const x = rnd() < 0.5 ? G.pick(ENCODABLE) : (() => {
            const n = Math.floor(rnd() * 64), a = new Uint8Array(n);
            for (let i = 0; i < n; i++) a[i] = Math.floor(rnd() * 256);
            return a;
        })();
        let wire, back;
        try { wire = enc(x); } catch (e) { bad = `${showArgs([x])} encode threw ${e && e.message}`; break; }
        if (encName === "Base58Encode" && wire.length > 4096) {
            let msg = "";
            try { dec(wire); } catch (e) { msg = e.message; }
            if (!/exceeds 4096/.test(msg)) bad = `a ${wire.length}-char base58 text must hit the documented 4096 cap, got ${JSON.stringify(msg)}`;
            continue;
        }
        try { back = dec(wire); } catch (e) { bad = `${showArgs([x])} decode threw ${e && e.message}`; break; }
        if (model && wire !== model(bytesOf(x))) { bad = `${showArgs([x])} encodes to ${JSON.stringify(wire)}, the spec gives ${JSON.stringify(model(bytesOf(x)))}`; break; }
        if (encName === "gzip" && !gzipFramed(wire, bytesOf(x))) { bad = `${showArgs([x])}: the gzip header or the CRC-32/ISIZE trailer is wrong`; break; }
        if (typeof x === "string" && keepsStrings) { if (back !== x) bad = `${showArgs([x])} -> ${showArgs([back])}`; continue; }
        if (!ArrayBuffer.isView(back) || !sameBytes(back, bytesOf(x))) bad = `${showArgs([x])} -> ${showArgs([back])}`;
    }
    if (bad) record(`${modName}.${encName}/${decName}`, `round trip lost data: ${bad} (seed ${SEED})`);
    else pass++;
}

const probeKey = "fuzz_pollution_probe";
if (({})[probeKey] === undefined) pass++;
else record("global", "the sweep reached Object.prototype");
if ([1, 2, 3].map((x) => x * 2).join(",") === "2,4,6") pass++;
else record("global", "the runtime is broken after the sweep");

print("\n" + "=".repeat(66));
if (skips.length) {
    print(`SKIPPED (${skips.length}) -- each states why:`);
    for (const s of skips.slice(0, 10)) print("  " + s);
    if (skips.length > 10) print(`  ... and ${skips.length - 10} more`);
}
if (fails.length) {
    print(`FAILURES (${fails.length}) -- replay with DYNA_FUZZ_SEED=${SEED}:`);
    for (const f of fails) print("  " + f);
}
print(`test_api_fuzz: ${calls} calls, ${pass} names clean, ${fail} failed, ${skipped} skipped, seed ${SEED}`);
await __kitS.section("block L224", async () => {
    const [names] = os.readdir(SCRATCH.dir);
    const made = names ? names.filter(n => n !== "." && n !== "..") : [];
    if (made.length)
        print(`NOTE: the fuzz sweep created ${made.length} filesystem entries from ` +
              `generated values: ${made.slice(0, 8).map(n => JSON.stringify(n)).join(" ")}` +
              (made.length > 8 ? " ..." : ""));
    scratchCleanup();
});
__kitS.end();
if (fail > 0) std.exit(1);
