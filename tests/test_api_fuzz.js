/* test_api_fuzz.js -- SEEDED property fuzzing over every exported name.
 *
 * The third layer, and the only one whose inputs nobody chose:
 *   test_api_params.js    hand-written tables      -- pins VALUES
 *   test_api_surface.js   a fixed argument matrix  -- pins BOUNDEDNESS
 *   this file             randomised arguments     -- finds the shape nobody thought of
 *
 * REPRODUCIBILITY IS THE WHOLE POINT. Every value comes from one seeded PRNG,
 * so a failure prints the seed and the exact argument tuple, and re-running
 * with DYNA_FUZZ_SEED=<n> replays it byte for byte. A fuzzer whose failures
 * cannot be replayed reports noise.
 *
 *   dynajs tests/test_api_fuzz.js                 # default seed, quick
 *   DYNA_FUZZ_SEED=12345 dynajs tests/test_api_fuzz.js
 *   DYNA_FUZZ_ROUNDS=200 dynajs tests/test_api_fuzz.js
 *
 * PROPERTIES ASSERTED (a fuzzer that only asks "did it crash" wastes its runs):
 *   P1 BOUNDED      no call exceeds its time budget
 *   P2 TOTAL        it returns or it throws -- never aborts, never hangs
 *   P3 CONTAINED    Object.prototype is untouched afterwards
 *   P4 PURE-ISH     the same seeded input twice gives the same answer, for the
 *                   families that promise determinism
 *   P5 ROUND TRIP   encode(decode(x)) == x wherever a pair exists
 *   P6 INTACT       the runtime still evaluates when the sweep is done
 */
import * as std from "std";
import * as os from "os";

/* Same blast-radius rule as test_api_surface.js, and it matters more here: this
 * sweep is RANDOMISED, so the paths it hands the file APIs are not even a fixed
 * list anyone reviewed. The CWD must not be the working tree. */
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
    os.chdir(SCRATCH.prev);
    const [names] = os.readdir(SCRATCH.dir);
    if (names) for (const n of names) if (n !== "." && n !== "..") os.remove(`${SCRATCH.dir}/${n}`);
    os.remove(SCRATCH.dir);
}

/* The generator lives in tests/fuzzgen.js and knows nothing about assertions,
   timing or reporting -- this file owns all of that. Sharing the data layer is
   what lets a pen test, a parametric test and this sweep drive the SAME corpus
   without inheriting each other's idea of what "failed" means. */
import { STRINGS, PRNG, generator, shrink, show } from "./fuzzgen.js";

/* A lone surrogate has no UTF-8 form, so decode(encode(x)) === x is undefined
   for it -- not a codec defect. Same exclusion as test_api_roundtrip.js. */
const lone = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:[^\uD800-\uDBFF]|^)[\uDC00-\uDFFF]/;
const ENCODABLE = STRINGS.filter(s => !lone.test(s));
const SEED = parseInt(std.getenv("DYNA_FUZZ_SEED") || "1337", 10);
const ROUNDS = parseInt(std.getenv("DYNA_FUZZ_ROUNDS") || "40", 10);
const BUDGET_MS = 1500;
const rnd = PRNG(SEED);
const G = generator(rnd);
const argsFor = () => G.args(3);
const showArgs = show;

/* -------------------------------------------------------------- the run */

const MODULES = ["bytes", "cli", "compress", "config", "crypto", "csv",
    "dataframe", "decimal", "encoding", "file", "hash", "html", "log",
    "matcher", "mathx", "ml", "net", "random", "scrape", "semver",
    "serialize", "simd", "structures", "sys", "time", "url", "uuid",
    "validate", "xml", "yaml"];

/* Same auditable skip list as test_api_surface.js: a name that binds a port,
   spawns, blocks or mutates the machine is not fuzzable in-process. */
const SKIP = {
    App: "binds a port", HTTPServer: "binds a port", HTTPServerAsync: "binds a port",
    TCPServer: "binds a port", UDPSocket: "binds a port", DNSServer: "binds a port",
    TCPProxy: "binds a port", HTTPClient: "network I/O", Fetcher: "network I/O",
    Crawl: "network I/O", DNSResolver: "network I/O", Redis: "connects out",
    PostgreSQL: "connects out", Watcher: "holds the loop open", Exec: "spawns",
    Which: "touches PATH", FileReader: "file handle", FileWriter: "file handle",
    File: "file handle", removeAll: "deletes", remove: "deletes",
    rename: "mutates the fs", move: "mutates the fs", symlink: "mutates the fs",
    chmod: "mutates the fs", makeDir: "mutates the fs", writeFile: "mutates the fs",
    writeFileAsync: "mutates the fs", chDir: "mutates the process",
    setEnv: "mutates the process", makeTempDir: "mutates the fs",
    makeTempFile: "mutates the fs",
};

/* Encode/decode pairs, for P5. Named explicitly because inferring them from
   the name would pair Base64Encode with Base32Decode. */
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

/* Families that promise the same answer for the same input (P4). */
const DETERMINISTIC = /^(SHA|MD5|BLAKE|Keccak|CRC|Murmur|Hex|Base|HMAC|Stable|compare|satisfies|major|minor|patch|isValid|erf|gcd|Levenshtein|Dice)/;

let pass = 0, fail = 0, skipped = 0, calls = 0;
const fails = [], skips = [];

function record(label, why) { fail++; fails.push(`${label}: ${why}`); }

function fuzzOne(label, fn, ctor, host, deterministic) {
    for (let round = 0; round < ROUNDS; round++) {
        const a = argsFor();
        let got, threw = null;
        const t0 = os.now();
        try { got = ctor ? new fn(...a) : fn.apply(host, a); }
        catch (e) { threw = e; }
        const dt = os.now() - t0;
        calls++;

        /* P1 BOUNDED -- shrunk before reporting, or the tuple is unreadable */
        if (dt >= BUDGET_MS) {
            const still = (t) => {
                const s0 = os.now();
                try { ctor ? new fn(...t) : fn.apply(host, t); } catch (e) {}
                return os.now() - s0 >= BUDGET_MS;
            };
            const min = shrink(a, still, 40);   /* small budget: each probe is slow */
            record(label, `took ${dt.toFixed(0)}ms; minimal input ${showArgs(min)} (seed ${SEED})`);
            return;
        }
        /* P2 TOTAL: reaching here means it returned or threw. A throw whose
           message is not a string is a defect in the error path itself. */
        if (threw && typeof threw.message !== "string" && threw instanceof Error) {
            record(label, `threw an Error with a non-string message on ${showArgs(a)}`);
            return;
        }
        /* P4 PURE-ISH */
        if (deterministic && !threw) {
            let again, threw2 = null;
            try { again = ctor ? new fn(...a) : fn.apply(host, a); }
            catch (e) { threw2 = e; }
            const same = threw2 ? false
                : (typeof got === "object" ? String(got) === String(again) : Object.is(got, again));
            if (!same && got === got /* skip NaN */) {
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
        /* release anything schedulable so the loop can still exit */
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
                fuzzOne(`dyna:${name}.${key}.${sub}`, v[sub], false, v,
                        DETERMINISTIC.test(sub));
                n++;
            }
        }
    }
    print(`  ${("dyna:" + name).padEnd(18)} ${String(n).padStart(4)} names fuzzed`);
}

/* ------------------------------------------------------- P5 round trips */

print("\n-- round trips: decode(encode(x)) must reproduce x --");
for (const [modName, encName, decName] of ROUND_TRIPS) {
    let m;
    try { m = await import("dyna:" + modName); } catch (e) { continue; }
    const enc = m[encName], dec = m[decName];
    if (typeof enc !== "function" || typeof dec !== "function") {
        skipped++; skips.push(`${modName}.${encName}/${decName}: absent`); continue;
    }
    let bad = null;
    for (let round = 0; round < ROUNDS && !bad; round++) {
        /* Only inputs the pair is DEFINED for: a codec is not obliged to
           round-trip a Symbol. Feeding it one tests the coercion, not the codec. */
        const x = rnd() < 0.5 ? G.pick(ENCODABLE) : (() => {
            const n = Math.floor(rnd() * 64), a = new Uint8Array(n);
            for (let i = 0; i < n; i++) a[i] = Math.floor(rnd() * 256);
            return a;
        })();
        let back;
        try { back = dec(enc(x)); } catch (e) { continue; }   /* refusal is fine */
        const lhs = typeof x === "string" ? x : Array.from(x).join(",");
        const rhs = typeof back === "string" ? back
            : (ArrayBuffer.isView(back) ? Array.from(back).join(",") : String(back));
        if (typeof x === "string" && typeof back !== "string") continue; /* byte form */
        if (lhs !== rhs) bad = `${showArgs([x])} -> ${showArgs([back])}`;
    }
    if (bad) record(`${modName}.${encName}/${decName}`, `round trip lost data: ${bad} (seed ${SEED})`);
    else pass++;
}

/* ------------------------------------------------------------- P3 and P6 */

const probeKey = "fuzz_pollution_probe";
if (({})[probeKey] === undefined) pass++;
else record("global", "the sweep reached Object.prototype");
if ([1, 2, 3].map((x) => x * 2).join(",") === "2,4,6") pass++;
else record("global", "the runtime is broken after the sweep");

/* ------------------------------------------------------------------ done */

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
{
    const [names] = os.readdir(SCRATCH.dir);
    const made = names ? names.filter(n => n !== "." && n !== "..") : [];
    if (made.length)
        print(`NOTE: the fuzz sweep created ${made.length} filesystem entries from ` +
              `generated values: ${made.slice(0, 8).map(n => JSON.stringify(n)).join(" ")}` +
              (made.length > 8 ? " ..." : ""));
    scratchCleanup();
}
if (fail > 0) std.exit(1);
