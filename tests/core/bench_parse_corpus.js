import * as std from "std";
import * as os from "os";

const MIN_BYTES = parseInt(scriptArgs[1] || "100000", 10);

function listFiles(dir) {
    const [names, err] = os.readdir(dir);
    if (err) return [];
    return names.filter(n => n.endsWith(".js")).map(n => dir + "/" + n);
}
const candidates = [...listFiles("tests"), ...listFiles("examples/js")];

function wrap(src, i) {
    const lines = src.split("\n");
    const out = [];
    for (let k = 0; k < lines.length; k++) {
        const t = lines[k].trimStart();
        if (t.startsWith("import ") || t.startsWith("import(")) {
            while (k < lines.length && lines[k].indexOf(";") < 0 &&
                   !/\bfrom\s+["']/.test(lines[k]))
                k++;
            continue;
        }
        if (t.startsWith("export default ")) { out.push(lines[k].replace("export default ", "")); continue; }
        if (t.startsWith("export ")) { out.push(lines[k].replace("export ", "")); continue; }
        out.push(lines[k]);
    }
    return "async function __corpus_" + i + "() {\n" + out.join("\n") + "\n}\n";
}

let corpus = "";
let used = 0, skipped = 0;
const parts = [];
for (let i = 0; i < candidates.length && corpus.length < MIN_BYTES * 3; i++) {
    const f = candidates[i];
    if (f.indexOf("bench_parse_corpus") >= 0) continue;
    let src;
    try { src = std.loadFile(f); } catch (e) { continue; }
    if (!src) continue;
    const w = wrap(src, used);
    try {
        eval("(function(){ " + w + " })");
        parts.push(w);
        corpus += w;
        used++;
    } catch (e) {
        skipped++;
    }
}

if (corpus.length < MIN_BYTES) {
    print("bench_parse_corpus: only " + corpus.length + " bytes of parsable corpus " +
          "(wanted " + MIN_BYTES + ") from " + used + " files");
}

const FEATURES = {
    "class": /\bclass\s+\w/g, "private field": /#\w+/g, "generator": /function\s*\*/g,
    "async": /\basync\b/g, "await": /\bawait\b/g, "arrow": /=>/g,
    "destructuring": /(?:const|let|var)\s*[[{]/g, "spread/rest": /\.\.\./g,
    "template": /`/g, "optional chain": /\?\./g, "nullish": /\?\?/g,
    "regexp literal": /[^\w)\]]\/(?![/*])(?:\\.|\[[^\]]*\]|[^/\n])+\//g,
    "getter/setter": /\b(?:get|set)\s+\w+\s*\(/g, "computed key": /\[[^\]]+\]\s*:/g,
    "try/catch": /\btry\s*{/g, "switch": /\bswitch\s*\(/g,
    "for-of": /\bfor\s*\(\s*(?:const|let|var)?[^;)]*\bof\b/g,
    "for-in": /\bfor\s*\(\s*(?:const|let|var)?[^;)]*\bin\b/g,
    "label": /^\s*\w+:\s*(?:for|while)\b/gm,
    "typed array": /\b(?:Uint8|Int32|Float64|Uint32|Int8|Uint16)Array\b/g,
    "Map/Set": /\bnew\s+(?:Map|Set|WeakMap|WeakSet)\b/g,
    "BigInt": /\d+n\b/g, "exponent": /\*\*/g,
};
print("bench_parse_corpus: " + corpus.length + " bytes from " + used +
      " real files (" + skipped + " skipped)");
{
    const found = [];
    for (const [name, re] of Object.entries(FEATURES)) {
        const n = (corpus.match(re) || []).length;
        if (n) found.push(name + "=" + n);
    }
    print("  features: " + found.join(" "));
}

const reps = parseInt(scriptArgs[2] || "12", 10);
let best = Infinity;
for (let run = 0; run < 3; run++) {
    const t0 = performance.now();
    for (let r = 0; r < reps; r++) eval(corpus);
    const dt = performance.now() - t0;
    if (dt < best) best = dt;
}
const mb = (corpus.length * reps) / (1024 * 1024);
print("#B corpus_parse " + best.toFixed(1) + "ms  " +
      (mb / (best / 1000)).toFixed(1) + " MB/s  (" + reps + " reps of " +
      (corpus.length / 1024).toFixed(0) + " KB)");
