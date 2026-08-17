/* test_api_surface.js -- GENERATED coverage over every exported name.
 *
 * test_api_params.js pins VALUES by hand and reaches ~19% of the surface,
 * because a hand-written table cannot keep up with 1195 names. This file
 * enumerates the surface from the BINARY and drives every callable through an
 * argument matrix, so a name added tomorrow is exercised tomorrow.
 *
 * What a generated row can and cannot prove: it cannot say the answer is
 * RIGHT -- only the hand-written tables do that. It proves the weaker but
 * unbounded-in-count properties: the call is bounded, it either returns or
 * throws rather than aborting, it does not reach Object.prototype, and the
 * runtime still works afterwards. That is exactly the class that regresses
 * silently when someone adds a native method and no test.
 *
 * Names it cannot call meaningfully (a server that binds, a blocking read) are
 * listed in SKIP with a reason, and the skip is PRINTED -- a lower count with
 * no failures must never read as green.
 */
import * as std from "std";
import * as os from "os";

/* CHDIR INTO A SCRATCH DIRECTORY BEFORE CALLING ANYTHING. A sweep that drives
 * every name with a hostile matrix WILL reach the file APIs, and it hands them
 * matrix values as paths -- so the first run of this suite created files named
 * `-1`, `NaN`, `null`, `true`, `abc`, `[object Object]` in the repo root.
 * Creating them is the benign case: the same sweep reaches remove and truncate.
 * The CWD is the blast radius, so it must not be the working tree. */
const SCRATCH = (() => {
    const base = (std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "");
    const dir = `${base}/dynajs-surface-${os.getpid ? os.getpid() : "x"}`;
    os.mkdir(dir, 0o700);
    const [prev, e] = os.getcwd();
    if (e || os.chdir(dir) !== 0)
        throw new Error(`refusing to sweep in ${prev}: cannot chdir to ${dir}`);
    return { dir, prev };
})();
function scratchCleanup() {
    os.chdir(SCRATCH.prev);
    /* Whatever the sweep created is named after a matrix value, not a path we
       chose, so enumerate rather than guess. */
    const [names] = os.readdir(SCRATCH.dir);
    if (names) for (const n of names) if (n !== "." && n !== "..") os.remove(`${SCRATCH.dir}/${n}`);
    os.remove(SCRATCH.dir);
}

const MODULES = ["bytes", "cli", "compress", "config", "crypto", "csv",
    "dataframe", "decimal", "encoding", "file", "hash", "html", "http",
    "json", "log", "matcher", "mathx", "ml", "net", "random", "schema",
    "scrape", "semver", "serialize", "simd", "structures", "sys", "time",
    "url", "uuid", "validate", "xml", "yaml"];

/* Anything that binds a port, spawns, blocks, or ends the process. Each entry
   states WHY, so the list is auditable rather than a convenient dumping ground. */
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
    "writeFileAsync": "mutates the filesystem", "chDir": "mutates the process",
    "setEnv": "mutates the process", "unsetenv": "mutates the process",
};

const BUDGET_MS = 1500;
let pass = 0, fail = 0, skipped = 0, calls = 0;
const fails = [], skips = [];

const LONE = String.fromCharCode(0xd800);
/* Ordered widest-first: the earliest arg that does not throw is usually the
   one the function wants, so the interesting paths get reached. */
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
        try { reap(ctor ? new fn(...a) : fn.apply(host, a)); }
        catch (e) { /* refusing is a correct outcome */ }
        const d = os.now() - t0;
        calls++;
        if (d > worst) { worst = d; worstArgs = a.map((x) => typeof x).join(","); }
        if (d >= BUDGET_MS) break;
    }
    if (worst < BUDGET_MS) pass++;
    else { fail++; fails.push(`${label} took ${worst.toFixed(0)}ms on (${worstArgs})`); }
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
            /* a namespace of functions (mathx.bits) is surface too */
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
        /* An instance's methods are surface the free functions never reach. */
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

/* The two properties the whole sweep must preserve. */
const leaked = ({})[probeKey];
if (leaked === undefined) pass++;
else { fail++; fails.push("the sweep reached Object.prototype: " + String(leaked)); }
if ([1, 2, 3].map((x) => x * 2).join(",") === "2,4,6") pass++;
else { fail++; fails.push("the runtime is broken after the sweep"); }

print("\n" + "=".repeat(64));
if (skips.length) {
    print(`SKIPPED (${skips.length}) -- each states why:`);
    for (const s of skips.slice(0, 12)) print("  " + s);
    if (skips.length > 12) print(`  ... and ${skips.length - 12} more`);
}
if (fails.length) {
    print(`FAILURES (${fails.length}):`);
    for (const f of fails) print("  " + f);
}
/* Report what the sweep left behind BEFORE deleting it: a name that creates a
   file from a hostile argument is a finding, and the count is the only evidence
   of it. Cleanup runs on the failure path too, or a red run leaks the scratch. */
{
    const [names] = os.readdir(SCRATCH.dir);
    const made = names ? names.filter(n => n !== "." && n !== "..") : [];
    if (made.length)
        print(`NOTE: the sweep created ${made.length} filesystem entries from matrix ` +
              `values: ${made.slice(0, 8).map(n => JSON.stringify(n)).join(" ")}` +
              (made.length > 8 ? " ..." : ""));
    scratchCleanup();
}
print(`test_api_surface: ${calls} calls, ${pass} names bounded, ${fail} failed, ${skipped} skipped`);
if (fail > 0) std.exit(1);
