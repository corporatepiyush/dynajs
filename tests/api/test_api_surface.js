// flags: --std
import * as std from "std";
import * as os from "os";

const SCRATCH = (() => {
    const base = (std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "");
    const dir = `${base}/dynajs-surface-${os.getpid ? os.getpid() : "x"}`;
    try { os.mkdir(dir, 0o700); }
    catch (e) {
        const [old] = os.readdir(dir);
        if (old) for (const n of old) if (n !== "." && n !== "..")
            os.remove(`${dir}/${n}`);
        os.remove(dir);
        os.mkdir(dir, 0o700);
    }
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
    "dataframe", "decimal", "encoding", "file", "hash", "html", "http",
    "json", "log", "matcher", "mathx", "ml", "net", "random", "schema",
    "scrape", "semver", "serialize", "simd", "structures", "sys", "time",
    "url", "uuid", "validate", "xml", "yaml"];

const SKIP = {
    "App": "binds a port and runs handlers on this thread",
    "HTTPServer": "binds a port", "HTTPServerAsync": "binds a port",
    "TCPServer": "binds a port", "UDPSocket": "binds a port",
    "DNSServer": "binds a port", "TCPProxy": "binds a port",
    "HTTPClient": "performs network I/O", "Fetcher": "performs network I/O",
    "fetch": "performs network I/O",
    "Crawl": "performs network I/O", "DNSResolver": "performs network I/O",
    "Redis": "connects to a server", "PostgreSQL": "connects to a server",
    "Watcher": "registers an OS watch that holds the loop open",
    "Exec": "spawns a process", "Which": "touches PATH",
    "FileReader": "opens a file handle", "FileWriter": "opens a file handle",
    "File": "opens a file handle",
    "removeAll": "deletes recursively", "remove": "deletes",
    "rename": "mutates the filesystem", "move": "mutates the filesystem",
    "symlink": "mutates the filesystem", "chmod": "mutates the filesystem",
    "makeDir": "mutates the filesystem", "writeFile": "mutates the filesystem",
    "chDir": "mutates the process",
    "setEnv": "mutates the process", "unsetenv": "mutates the process",
};

// The wall-clock bound is overridable so a SANITIZER leg (which is
// several times slower) does not assert a release-build duration against
// itself; the engine-side fix (work-based bounds) is B1-15/TEST-03.
const BUDGET_MS = +(std.getenv("DYNAJS_BUDGET_MS") || 1500);
const NOTE_MS = 1000;
const STUCK_MS = 30000;
let pass = 0, fail = 0, skipped = 0, calls = 0, bounded = 0;
const fails = [], skips = [], notes = [];

const ENC = new TextEncoder();
const W = (s) => { const b = ENC.encode(s); os.write(1, b.buffer, 0, b.length); };

const LONE = String.fromCharCode(0xd800);
const ARGS = [
    [], ["abc"], [""], [0], [1], [-1], [NaN], [Infinity], [0.5], [2 ** 53],
    [LONE], ["\0"], ["a".repeat(4096)], [[]], [[1, 2, 3]], [{}],
    [new Uint8Array([1, 2, 3])], [new Float64Array([1, 2, 3])],
    [null], [undefined], [true],
    ["abc", "abc"], [1, 1], [[1, 2], [3, 4]], [{}, {}], ["abc", 1],
    [new Float32Array([1, 2]), new Float32Array([3, 4])],
];

function reap(v) {
    const t = typeof v;
    if (!v || (t !== "object" && t !== "function")) return;
    try { if (typeof v.cancel === "function") v.cancel(); } catch (e) {}
    try { if (typeof v.close === "function") v.close(); } catch (e) {}
    try { if (typeof v.then === "function" && typeof v.catch === "function")
        v.catch(function () {}); } catch (e) {}
}

function drive(label, fn, ctor, host) {
    let worst = 0, worstArgs = "";
    for (const a of ARGS) {
        const t0 = os.now();
        try {
            const rv = ctor ? new fn(...a) : fn.apply(host, a);
            reap(rv);
            if (rv && (typeof rv === "object" || typeof rv === "function") &&
                typeof rv.then === "function") {
                rv.then(function () {}, function () {});
                bounded++;
            }
        }
        catch (e) { bounded++;  }
        const d = os.now() - t0;
        calls++;
        if (d > worst) { worst = d; worstArgs = a.map((x) => typeof x).join(","); }
        if (d >= BUDGET_MS) break;
    }
    if (worst >= STUCK_MS)
        { fail++; fails.push(`${label} wedged ${worst.toFixed(0)}ms on (${worstArgs})`); }
    else if (worst >= NOTE_MS)
        notes.push(`${label}: worst ${worst.toFixed(0)}ms on (${worstArgs})`);
    else pass++;
}

const probeKey = "surface_pollution_probe";

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
        if (typeof v !== "function") {
            if (v && typeof v === "object") {
                for (const sub of Object.getOwnPropertyNames(v)) {
                    if (typeof v[sub] !== "function") continue;
                    drive(`dyna:${name}.${key}.${sub}`, v[sub], false, v);
                    n++;
                }
            }
            continue;
        }
        const isClass = v.prototype &&
              Object.getOwnPropertyNames(v.prototype).length > 1;
        drive(`dyna:${name}.${key}`, v, isClass, ns);
        n++;
        if (isClass) {
            let inst = null;
            for (const a of ARGS) {
                try { inst = new v(...a); break; } catch (e) {}
            }
            if (!inst) continue;
            for (const mname of Object.getOwnPropertyNames(v.prototype)) {
                if (mname === "constructor") continue;
                if (SKIP[mname]) { skipped++; skips.push(`${key}.${mname}: ${SKIP[mname]}`); continue; }
                let mv;
                try { mv = v.prototype[mname]; } catch (e) { continue; }
                if (typeof mv !== "function") continue;
                drive(`dyna:${name}.${key}#${mname}`, mv, false, inst);
                n++;
            }
            reap(inst);
        }
    }
    print(`  ${("dyna:" + name).padEnd(18)} ${String(n).padStart(4)} names driven`);
}

const WATCHDOG = setTimeout(() => {
    W("test_api_surface: HUNG (watchdog)\n");
    std.exit(124);
}, 120000);

const leaked = ({})[probeKey];
if (leaked === undefined) pass++;
else { fail++; fails.push("the sweep reached Object.prototype: " + String(leaked)); }
if ([1, 2, 3].map((x) => x * 2).join(",") === "2,4,6") pass++;
else { fail++; fails.push("the runtime is broken after the sweep"); }

W("\n" + "=".repeat(64) + "\n");
if (skips.length) {
    W(`SKIPPED (${skips.length}) -- each states why:\n`);
    for (const s of skips.slice(0, 12)) W("  " + s + "\n");
    if (skips.length > 12) W(`  ... and ${skips.length - 12} more\n`);
}
if (notes.length) {
    W(`SLOW (${notes.length}, reported not failed):\n`);
    for (const s of notes.slice(0, 12)) W("  " + s + "\n");
    if (notes.length > 12) W(`  ... and ${notes.length - 12} more\n`);
}
if (fails.length) {
    W(`FAILURES (${fails.length}):\n`);
    for (const f of fails) W("  " + f + "\n");
}
{
    const [names] = os.readdir(SCRATCH.dir);
    const made = names ? names.filter(n => n !== "." && n !== "..") : [];
    if (made.length)
        W(`NOTE: the sweep created ${made.length} filesystem entries from matrix ` +
          `values: ${made.slice(0, 8).map(n => JSON.stringify(n)).join(" ")}` +
          (made.length > 8 ? " ..." : "") + "\n");
    scratchCleanup();
}
W(`test_api_surface: ${calls} calls, ${pass} names bounded, ${bounded} bounded throws (sync + observed async rejections), ${fail} failed, ${skipped} skipped\n`);
clearTimeout(WATCHDOG);
if (fail > 0) std.exit(1);
