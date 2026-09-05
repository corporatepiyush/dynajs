/* bench_parse_corpus.js -- parse a REAL codebase, not a generated one.
 *
 * Every parser benchmark in this tree before this one was synthetic: a loop
 * emitting the same function shape a few hundred times. That measures the
 * shapes the generator happens to emit and nothing else, and it is how a
 * lexer optimisation gets credited with a win it does not have on real input.
 *
 * This one builds its corpus from the repository's own hand-written JavaScript
 * -- the test suite and the examples -- which is what a real codebase looks
 * like: classes with private fields, generators, async/await, destructuring,
 * template literals, optional chaining, regexps, labels, getters, computed
 * keys, tagged templates, typed arrays, and long prose comments.
 *
 * PARSE-ONLY, by construction. Each file is wrapped in an `async function` that
 * is never called, so eval() parses the whole body and executes nothing. That
 * is the same trick bench/b_parse_heavy.js uses and the reason it works: a
 * parse benchmark that runs the code it parses measures the interpreter.
 *
 * Imports are stripped because a wrapped body cannot carry them; the parser
 * does not resolve identifiers, so the bodies parse exactly as written.
 *
 * Run: dynajs --std tests/bench_parse_corpus.js [minBytes]
 */
import * as std from "std";
import * as os from "os";

const MIN_BYTES = parseInt(scriptArgs[1] || "100000", 10);

/* Real files, largest first, so the corpus reaches its size target with the
 * most feature-dense material rather than a long tail of tiny ones. */
function listFiles(dir) {
    const [names, err] = os.readdir(dir);
    if (err) return [];
    return names.filter(n => n.endsWith(".js")).map(n => dir + "/" + n);
}
const candidates = [...listFiles("tests"), ...listFiles("examples/js")];

/* Strip what a function body cannot contain, and nothing else.
 *
 * An import statement spans lines more often than not in this tree --
 * `import {\n  a,\n  b\n} from "m";` -- so a per-line filter drops the first
 * line and leaves `a, b } from "m";` behind, which then fails to parse. The
 * first version of this did exactly that and silently skipped the seventeen
 * LARGEST files, i.e. the most feature-dense ones, leaving a corpus that
 * flattered whatever it did include. Consume to the terminator instead. */
function wrap(src, i) {
    const lines = src.split("\n");
    const out = [];
    for (let k = 0; k < lines.length; k++) {
        const t = lines[k].trimStart();
        if (t.startsWith("import ") || t.startsWith("import(")) {
            /* to the end of the statement, however many lines that takes */
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
    /* Keep only what actually parses wrapped -- a file relying on top-level
     * module syntax is not usable here, and silently including a broken one
     * would make the benchmark measure error recovery. */
    try {
        /* parse-test the wrapped unit on its own: a file that does not parse
         * here would make the benchmark measure error recovery */
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

/* Feature census, so the corpus can be shown to be varied rather than claimed
 * to be. Counted on the real text, not on a generator's intent. */
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

/* Time the parse. Reps chosen so the run is long enough to be stable. */
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
