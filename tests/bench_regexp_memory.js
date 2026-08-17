/* bench_regexp_memory.js -- how many bytes of backtrack stack the regex engine
 * needs per input character.
 *
 * WHY THIS IS A SEPARATE FILE, AND WHY IT FORKS.
 *
 * `ru_maxrss` is a high-water mark over a PROCESS. It is a per-operation number
 * only when the process performs the operation once. Neither condition holds
 * inside a sweep, and both failures point the same way -- too big:
 *
 *   - it is a running max, so a row silently inherits any peak set by an
 *     earlier row;
 *   - it grows LINEARLY WITH THE NUMBER OF EXECUTIONS. One exec of
 *     /^[\s\S]*NEEDLE/ over 1 M characters moves it 25 MB; 8 execs move it
 *     195 MB; 32 move it 771 MB -- while the allocator's peak LIVE bytes stays
 *     constant at 31 MB and every block is freed (libregexp.c:3650 releases the
 *     stack on every exit path). The pages are simply not reused.
 *
 * That second effect is not this engine's: a plain malloc ladder with no regex
 * in it -- grow 256 B to 24 MB by realloc, free, repeat -- reproduces the
 * figures to the kilobyte. Changing libregexp's growth factor from 1.5x to 2x
 * moves them by 0.04%, which is what rules out the realloc ladder as the cause.
 *
 * An earlier memory column in tests/bench_regexp_scale.js reported 195 bytes
 * per character on this basis. The true figure is 24, and the difference was
 * the rep count. Hence: one child process per measurement, exactly one exec.
 *
 * Output lines beginning with `#M` are machine-readable:
 *   #M <case> <chars> <delta_bytes> <bytes_per_char>
 *
 * Usage: dynajs --std tests/bench_regexp_memory.js [--exe PATH]
 *        dynajs --std tests/bench_regexp_memory.js --child <case> <chars> <out>
 */
import * as std from "std";
import * as os from "os";

/* Each case is built by name in the child, because a regexp cannot cross a
   process boundary. Keep the names in step with bench_regexp_scale.js. */
function patternFor(name) {
    switch (name) {
    case "literal_scan": return /NEEDLE_AT_THE_VERY_END/;
    case "class_scan":   return /N[A-Z]{5}_AT_THE/;
    case "dot_star":     return /^[\s\S]*NEEDLE/;
    case "alternation":  return /(NEEDLE|HAYSTACK|MISSING)_AT_THE/;
    /* Greedy `.` and greedy `[\s\S]` are the same shape -- one split frame per
       position -- and both must show the linear cost. */
    case "greedy_dot":   return /^.*NEEDLE/;
    /* The lazy twin. It reaches the same match from the other end, so any
       difference in stack is the quantifier's, not the pattern's. */
    case "lazy_dot_star": return /^[\s\S]*?NEEDLE/;
    }
    return null;
}

const UNIT = "the quick brown fox jumps over the lazy dog 0123456789 ";
const TAIL = " NEEDLE_AT_THE_VERY_END";

function mkSubject(chars) {
    const body = Math.max(0, chars - TAIL.length);
    const s = body > 0 ? UNIT.repeat(Math.ceil(body / UNIT.length)).slice(0, body) : "";
    return s + TAIL;
}

/* ---- child: one subject, one exec, one number ---------------------------- */

const argv = scriptArgs;
if (argv.indexOf("--child") >= 0) {
    const i = argv.indexOf("--child");
    const name = argv[i + 1], chars = parseInt(argv[i + 2], 10), out = argv[i + 3];
    const sys = await import("dyna:sys");
    const re = patternFor(name);
    const subj = mkSubject(chars);
    /* Touch the string so it is fully materialised before the baseline: a rope
       flattened inside the timed region would be charged to the regex. */
    if (subj.charCodeAt(0) === -1) print("unreachable");
    const before = sys.memoryUsage().peakRss;
    re.lastIndex = 0;
    const matched = re.test(subj);
    const after = sys.memoryUsage().peakRss;
    const f = std.open(out, "w");
    f.puts((after - before) + " " + (matched ? 1 : 0) + "\n");
    f.close();
    std.exit(0);
}

/* ---- parent: fork one child per (case, size) ----------------------------- */

const ei = argv.indexOf("--exe");
const EXE = ei >= 0 ? argv[ei + 1]
                    : (os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs");
const SELF = argv[0];
const TMP = "/tmp/_dyna_re_mem.txt";

/* `matched` is printed for every row because it is the check that a row
   measured anything: a pattern that fails on the first character allocates
   nothing and reports a flattering 0 B/char. */
const CASES = ["literal_scan", "class_scan", "dot_star", "greedy_dot",
               "lazy_dot_star", "alternation"];
const SIZES = [1000000, 2000000, 4000000];

function measure(name, chars) {
    const rc = os.exec([EXE, "--std", SELF, "--child", name, String(chars), TMP],
                       { usePath: true });
    if (rc !== 0) return null;
    const f = std.open(TMP, "r");
    if (!f) return null;
    const line = f.readAsString().trim();
    f.close();
    const parts = line.split(" ");
    return { bytes: parseInt(parts[0], 10), matched: parts[1] === "1" };
}

/* The probe is only meaningful with dyna:sys, and the child cannot report its
   absence usefully -- check once, here, and refuse rather than print zeros. */
try {
    await import("dyna:sys");
} catch (e) {
    print("bench_regexp_memory: needs dyna:sys (build CONFIG_NATIVE_MODULES=y)");
    std.exit(1);
}

print("bench_regexp_memory: one child process per row, exactly one exec each");
print("");
print("  case             chars     deltaKB   B/char  matched");

for (const name of CASES) {
    for (const chars of SIZES) {
        const r = measure(name, chars);
        if (!r) { print("  " + name.padEnd(15) + " FAILED"); continue; }
        print("  " + name.padEnd(15) + String(chars).padStart(9) +
              String(Math.round(r.bytes / 1024)).padStart(11) +
              (r.bytes / chars).toFixed(2).padStart(9) +
              String(r.matched).padStart(9));
        print("#M " + name + " " + chars + " " + r.bytes + " " +
              (r.bytes / chars).toFixed(3));
    }
}
os.remove(TMP);
print("#M DONE");
