import * as std from "std";
import * as os from "os";

function patternFor(name) {
    switch (name) {
    case "literal_scan": return /NEEDLE_AT_THE_VERY_END/;
    case "class_scan":   return /N[A-Z]{5}_AT_THE/;
    case "dot_star":     return /^[\s\S]*NEEDLE/;
    case "alternation":  return /(NEEDLE|HAYSTACK|MISSING)_AT_THE/;
    case "greedy_dot":   return /^.*NEEDLE/;
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

const argv = scriptArgs;
if (argv.indexOf("--child") >= 0) {
    const i = argv.indexOf("--child");
    const name = argv[i + 1], chars = parseInt(argv[i + 2], 10), out = argv[i + 3];
    const sys = await import("dyna:sys");
    const re = patternFor(name);
    const subj = mkSubject(chars);
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

const ei = argv.indexOf("--exe");
const EXE = ei >= 0 ? argv[ei + 1]
                    : (os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs");
const SELF = argv[0];
const TMP = `${std.getenv("TMPDIR") || "/tmp"}/dj_re_mem.${Date.now() % 10000000}.txt`;

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
