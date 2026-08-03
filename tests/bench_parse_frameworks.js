/* bench_parse_frameworks.js -- parse REAL shipped frameworks, minified and not.
 *
 * RUN THIS BEFORE AND AFTER ANY CHANGE TO THE PARSER OR LEXER.
 *
 * tests/bench_parse_corpus.js assembles ~295 KB from this repository's own
 * hand-written JavaScript. That is honest code, but it is all one dialect:
 * moderate line lengths, real comments, descriptive identifiers. It cannot see
 * a regression that only shows up on a minified bundle -- one 100 KB line, 1-2
 * character identifiers, almost no whitespace, and expression nesting that
 * hand-written code never reaches. Those are opposite ends of the lexer, and a
 * change can help one while costing the other.
 *
 * This benchmark parses the corpus fetched by tests/fetch_frameworks.sh:
 * React, Vue, Angular, jQuery, three.js, d3, lodash, moment, ECharts,
 * Bootstrap, PixiJS, Chart.js on the client side; the TypeScript compiler,
 * Babel, Prettier, Terser, acorn, axios, socket.io and core-js on the server
 * side. Each is present minified and unminified where the project ships both.
 *
 * PARSE-ONLY, by construction: `new Function(src)` compiles the source as a
 * function body and never calls it. This engine has no lazy parsing, so the
 * whole body is really parsed and compiled. A UMD/CommonJS bundle is a valid
 * function body (top-level `return` is legal there); an ES module is not, which
 * is why the fetch manifest contains no ESM builds.
 *
 * Usage:
 *   dynajs --std tests/bench_parse_frameworks.js            # default set
 *   dynajs --std tests/bench_parse_frameworks.js --all      # include SLOW files
 *   dynajs --std tests/bench_parse_frameworks.js --budget 8000
 *
 * Output lines beginning with `#B` are machine-readable:
 *   #B <name> <kind> <bytes> <ms> <MB/s> <status>
 */
import * as std from "std";
import * as os from "os";

const DIR = "bench/frameworks";

/* Files whose parse time is pathological and unrelated to lexer throughput.
 * Measured 2026-07-28 on an otherwise-idle machine: each exceeds 25 s, while
 * the TypeScript compiler -- 8.9 MB, the largest file in the corpus -- parses
 * in 394 ms and prettier.dev.js at 75 KB does not finish. Size is not the
 * variable; see PARSE_PATHOLOGY.md. They are excluded by default so a routine
 * before/after comparison is not dominated by them, and included by --all
 * because the cause is a real defect worth fixing. */
const SLOW = ["babel.dev.js", "babel.min.js", "bootstrap.dev.js",
              "echarts.dev.js", "echarts.min.js", "prettier.dev.js"];

const args = scriptArgs.slice(1);
const ALL = args.includes("--all");
const bi = args.indexOf("--budget");
/* Per-file wall-clock budget. A file that blows it is reported, not silently
 * dropped -- a benchmark that hides what it could not measure is flattering
 * itself (see the corpus-builder note in tests/bench_parse_corpus.js). */
const BUDGET_MS = bi >= 0 ? parseInt(args[bi + 1], 10) : 20000;

function listFiles(dir) {
    const [names, err] = os.readdir(dir);
    if (err) return [];
    return names.filter(n => n.endsWith(".js")).sort();
}

const names = listFiles(DIR);
if (names.length === 0) {
    print("no corpus: run tests/fetch_frameworks.sh first");
    std.exit(1);
}

/* Classify from the filename the fetch script assigns. */
function kindOf(n) {
    const server = ["typescript", "babel", "prettier", "terser", "acorn",
                    "axios", "socketio", "corejs"];
    return server.some(s => n.startsWith(s)) ? "server" : "client";
}
function formOf(n) { return n.includes(".min.") ? "min" : "dev"; }

/* Parse one source and return {ms, status}. Everything the parser can throw is
 * caught, including stack overflow: a deeply nested minified expression is
 * exactly the input that reaches the recursion limit, and the run must report
 * that rather than abort the whole benchmark. */
function parseOnce(src) {
    const t0 = performance.now();
    let status = "ok";
    try {
        new Function(src);
    } catch (e) {
        /* InternalError "stack overflow" is the one we care about naming. */
        const m = String(e && e.message || e);
        status = (m.indexOf("stack overflow") >= 0)
            ? "STACK-OVERFLOW"
            : (e && e.constructor ? e.constructor.name : "error") + ":" +
              m.slice(0, 40).replace(/\s+/g, " ");
    }
    return { ms: performance.now() - t0, status };
}

print("bench_parse_frameworks: " + names.length + " files in " + DIR +
      (ALL ? "  [--all]" : "  [skipping " + SLOW.length + " pathological]") +
      "  budget=" + BUDGET_MS + "ms/file");
print("");
print("  file                    kind   form      KB      ms      MB/s  status");

let totBytes = 0, totMs = 0, skipped = 0, failed = 0;
const byGroup = {};

for (const n of names) {
    if (!ALL && SLOW.indexOf(n) >= 0) { skipped++; continue; }
    const src = std.loadFile(DIR + "/" + n);
    if (src === null) { print("  LOAD-FAIL " + n); failed++; continue; }

    /* One warm pass so the atom table and the constant pool are populated,
     * then the timed pass. Both are full parses; nothing is cached between
     * them at the parser level, but the allocator arenas settle. */
    const warm = parseOnce(src);
    if (warm.ms > BUDGET_MS) {
        print("  " + n.padEnd(22) + " OVER BUDGET (" + warm.ms.toFixed(0) +
              " ms) -- add to SLOW or raise --budget");
        failed++;
        continue;
    }
    const r = parseOnce(src);

    const bytes = src.length;
    const mbps = (bytes / (1024 * 1024)) / (r.ms / 1000);
    const kind = kindOf(n), form = formOf(n);
    print("  " + n.padEnd(22) + " " + kind.padEnd(6) + " " + form.padEnd(4) +
          String((bytes / 1024).toFixed(0)).padStart(7) +
          String(r.ms.toFixed(1)).padStart(8) +
          String(mbps.toFixed(1)).padStart(10) + "  " + r.status);
    print("#B " + n + " " + kind + " " + form + " " + bytes + " " +
          r.ms.toFixed(3) + " " + mbps.toFixed(2) + " " + r.status);

    if (r.status !== "ok") failed++;
    totBytes += bytes; totMs += r.ms;
    const g = kind + "/" + form;
    byGroup[g] = byGroup[g] || { bytes: 0, ms: 0, n: 0 };
    byGroup[g].bytes += bytes; byGroup[g].ms += r.ms; byGroup[g].n++;
}

print("");
print("  group          files        KB       ms      MB/s");
for (const g of Object.keys(byGroup).sort()) {
    const v = byGroup[g];
    print("  " + g.padEnd(14) + String(v.n).padStart(5) +
          String((v.bytes / 1024).toFixed(0)).padStart(10) +
          String(v.ms.toFixed(1)).padStart(9) +
          String(((v.bytes / (1024 * 1024)) / (v.ms / 1000)).toFixed(1)).padStart(10));
    print("#G " + g + " " + v.n + " " + v.bytes + " " + v.ms.toFixed(3) + " " +
          ((v.bytes / (1024 * 1024)) / (v.ms / 1000)).toFixed(2));
}
print("");
print("#B TOTAL all all " + totBytes + " " + totMs.toFixed(3) + " " +
      ((totBytes / (1024 * 1024)) / (totMs / 1000)).toFixed(2) + " " +
      (failed ? failed + "-failed" : "ok"));
print("bench_parse_frameworks: " + (totBytes / 1024 / 1024).toFixed(2) + " MB in " +
      totMs.toFixed(1) + " ms  (" +
      ((totBytes / (1024 * 1024)) / (totMs / 1000)).toFixed(1) + " MB/s), " +
      "skipped=" + skipped + " failed=" + failed);
