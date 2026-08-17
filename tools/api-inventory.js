/*
 * API inventory: every public name reachable from the BINARY, and whether any
 * test, example or fuzz target mentions it.
 *
 * Native members are non-enumerable, so Object.keys reports nothing for them --
 * this walks getOwnPropertyNames plus the prototype chain. Granularity is the
 * METHOD, not the class: an earlier sweep at class granularity found 0 gaps
 * while the same sweep at method granularity found a route handler with no test
 * of any kind.
 *
 *   dynajs tools/api-inventory.js            # human summary
 *   dynajs tools/api-inventory.js --json     # machine readable
 *   dynajs tools/api-inventory.js --uncovered
 */
import * as std from "std";
import * as os from "os";

const MODULES = [
    "bytes", "cli", "compress", "config", "crypto", "csv", "dataframe",
    "decimal", "encoding", "file", "hash", "html", "http", "log", "matcher",
    "mathx", "ml", "net", "random", "scrape", "semver", "serialize", "simd",
    "structures", "sys", "term", "time", "uring", "url", "uuid", "validate",
    "xml", "yaml",
];

/* Builtin prototypes carrying this project's extension methods. */
const EXT_HOSTS = [
    ["String", String], ["Array", Array], ["Number", Number],
    ["Object", Object], ["Math", Math], ["Date", Date],
    ["Promise", Promise], ["RegExp", RegExp], ["Map", Map], ["Set", Set],
];

const SKIP = new Set([
    "constructor", "length", "name", "prototype", "caller", "arguments",
    "__proto__", "toString", "valueOf", "toLocaleString",
]);

function isPlainObject(v) {
    return v !== null && typeof v === "object";
}

/* every own name on obj and its prototype chain, minus Object.prototype */
function ownNames(obj, depth) {
    const out = [];
    let seen = new Set();
    let o = obj, d = 0;
    while (o && o !== Object.prototype && o !== Function.prototype && d < (depth || 4)) {
        for (const n of Object.getOwnPropertyNames(o)) {
            if (SKIP.has(n) || seen.has(n)) continue;
            seen.add(n);
            out.push(n);
        }
        o = Object.getPrototypeOf(o);
        d++;
    }
    return out;
}

function kindOf(container, name) {
    let d;
    try {
        d = Object.getOwnPropertyDescriptor(container, name);
    } catch (e) {
        return "unknown";
    }
    if (!d) return "unknown";
    if (d.get || d.set) return "accessor";
    if (typeof d.value === "function") return "function";
    return "value";
}

/* ---------------------------------------------------------------- collect */

const entries = [];   /* {module, symbol, kind, id} */

function addClass(mod, className, ctor) {
    for (const n of Object.getOwnPropertyNames(ctor)) {
        if (SKIP.has(n)) continue;
        entries.push({
            module: mod, symbol: className + "." + n,
            kind: "static-" + kindOf(ctor, n), id: n,
        });
    }
    const proto = ctor.prototype;
    if (!proto) return;
    for (const n of ownNames(proto)) {
        entries.push({
            module: mod, symbol: className + ".prototype." + n,
            kind: kindOf(proto, n), id: n,
        });
    }
}

async function collectModules() {
    const missing = [];
    for (const m of MODULES) {
        let ns;
        try {
            ns = await import("dyna:" + m);
        } catch (e) {
            missing.push(m);
            continue;
        }
        for (const name of Object.getOwnPropertyNames(ns)) {
            if (name === "default" || name === "__esModule") continue;
            let v;
            try { v = ns[name]; } catch (e) { continue; }
            const isClass = typeof v === "function" && v.prototype &&
                  Object.getOwnPropertyNames(v.prototype).length > 1;
            if (isClass) {
                addClass("dyna:" + m, name, v);
                entries.push({ module: "dyna:" + m, symbol: name,
                               kind: "class", id: name });
            } else if (typeof v === "function") {
                entries.push({ module: "dyna:" + m, symbol: name,
                               kind: "function", id: name });
            } else if (isPlainObject(v)) {
                entries.push({ module: "dyna:" + m, symbol: name,
                               kind: "namespace", id: name });
                for (const sub of ownNames(v, 2)) {
                    entries.push({ module: "dyna:" + m, symbol: name + "." + sub,
                                   kind: kindOf(v, sub), id: sub });
                }
            } else {
                entries.push({ module: "dyna:" + m, symbol: name,
                               kind: "value", id: name });
            }
        }
    }
    return missing;
}

function collectExtensions() {
    for (const [hostName, host] of EXT_HOSTS) {
        for (const n of Object.getOwnPropertyNames(host)) {
            if (SKIP.has(n)) continue;
            entries.push({ module: "ext:" + hostName, symbol: hostName + "." + n,
                           kind: "static-" + kindOf(host, n), id: n });
        }
        const proto = host.prototype;
        if (!proto) continue;
        for (const n of Object.getOwnPropertyNames(proto)) {
            if (SKIP.has(n)) continue;
            entries.push({
                module: "ext:" + hostName,
                symbol: hostName + ".prototype." + n,
                kind: kindOf(proto, n), id: n,
            });
        }
    }
}

/* ------------------------------------------------------------- coverage */

/* Read every test/example/fuzz source once; a name is "covered" if it is
   mentioned as a word anywhere in them. This is deliberately generous: it
   measures whether anybody ever THOUGHT about the name, which is a different
   and cheaper question than line coverage. */
function loadCorpus() {
    const dirs = ["tests", "examples", "fuzz"];
    let text = "";
    let files = 0;
    for (const d of dirs) {
        const [names, err] = os.readdir(d);
        if (err !== 0) continue;
        for (const n of names) {
            if (n === "." || n === "..") continue;
            if (!/\.(js|mjs|c|h|py|sh)$/.test(n)) continue;
            const body = std.loadFile(d + "/" + n);
            if (body === null) continue;
            text += body + "\n";
            files++;
        }
    }
    return { text, files };
}

const ADVERSARIAL = /pentest|traversal|attack|adversar|malicious|smuggl|bomb|overflow|fuzz/i;

function loadAdversarialCorpus() {
    const dirs = ["tests", "fuzz"];
    let text = "";
    const files = [];
    for (const d of dirs) {
        const [names, err] = os.readdir(d);
        if (err !== 0) continue;
        for (const n of names) {
            if (!/\.(js|mjs|c|py)$/.test(n)) continue;
            const body = std.loadFile(d + "/" + n);
            if (body === null) continue;
            /* a file counts as adversarial by NAME or by declared intent */
            if (ADVERSARIAL.test(n) || ADVERSARIAL.test(body.slice(0, 2000))) {
                text += body + "\n";
                files.push(d + "/" + n);
            }
        }
    }
    return { text, files };
}

/* test262 is laid out as test/built-ins/<Host>/prototype/<method>/, so its
   DIRECTORY names are exactly the ES-standard surface -- free, and it tracks
   the pinned version instead of a hand-written list that would rot. The set
   is PER HOST: a flat set of bare names let Math.min mark Array.min as
   standard and both went undocumented. */
function standardNames() {
    const std262 = new Map();
    for (const [hostName] of EXT_HOSTS) {
        const set = new Set();
        std262.set(hostName, set);
        for (const p of ["test262/test/built-ins/" + hostName,
                         "test262/test/built-ins/" + hostName + "/prototype"]) {
            const [names, err] = os.readdir(p);
            if (err !== 0) continue;
            for (const n of names) {
                if (n === "." || n === ".." || /\.js$/.test(n)) continue;
                set.add(n);
            }
        }
    }
    return std262;
}

/* tc39 stage-3 proposals ship in the pinned test262 checkout, so the
   directory scan marks them standard and the reference skips them -- while
   this engine implements and documents them as extensions. Name them here so
   the coverage gates hold their docs and types accountable like every other
   export. */
const STAGE3_DOCUMENTED = new Set(["getOrInsert", "getOrInsertComputed"]);

function mentions(corpus, id) {
    /* \b is not a word boundary in POSIX ERE, and this is JS RegExp, but the
       trap it guards against is real: build the pattern explicitly. */
    let from = 0;
    for (;;) {
        const i = corpus.indexOf(id, from);
        if (i < 0) return false;
        const before = i === 0 ? "" : corpus[i - 1];
        const after = corpus[i + id.length] || "";
        if (!/[A-Za-z0-9_$]/.test(before) && !/[A-Za-z0-9_$]/.test(after))
            return true;
        from = i + 1;
    }
}

/* ------------------------------------------------------------------ main */

const args = scriptArgs.slice(1);
const wantJson = args.includes("--json");
const wantUncovered = args.includes("--uncovered");

const missing = await collectModules();
collectExtensions();

const corpus = loadCorpus();
const adv = loadAdversarialCorpus();

const std262 = standardNames();
for (const e of entries) {
    e.tested = mentions(corpus.text, e.id);
    e.adversarial = mentions(adv.text, e.id);
    /* an ES-standard name is covered by test262, which is not in the corpus:
       counting it as a gap is noise that buries the real ones */
    e.standard = e.module.startsWith("ext:") &&
                 !STAGE3_DOCUMENTED.has(e.id) &&
                 (std262.get(e.module.slice(4))?.has(e.id) ?? false);
}

/* per-module rollup */
const byModule = new Map();
for (const e of entries) {
    let r = byModule.get(e.module);
    if (!r) {
        r = { module: e.module, total: 0, tested: 0, adversarial: 0, gaps: [] };
        byModule.set(e.module, r);
    }
    if (e.standard) { r.standard = (r.standard || 0) + 1; continue; }
    r.total++;
    if (e.tested) r.tested++; else r.gaps.push(e.symbol);
    if (e.adversarial) r.adversarial++;
}

if (wantJson) {
    console.log(JSON.stringify({
        generated_from: "binary",
        modules_missing: missing,
        corpus_files: corpus.files,
        adversarial_files: adv.files,
        entries,
        rollup: [...byModule.values()],
    }, null, 1));
} else if (wantUncovered) {
    for (const r of [...byModule.values()].sort((a, b) => b.gaps.length - a.gaps.length)) {
        if (!r.gaps.length) continue;
        console.log("\n" + r.module + "  (" + r.gaps.length + " unreferenced)");
        for (const g of r.gaps) console.log("   " + g);
    }
} else {
    const pad = (s, n) => String(s).padEnd(n);
    console.log("corpus: " + corpus.files + " test/example/fuzz files, " +
                adv.files.length + " adversarial");
    if (missing.length)
        console.log("modules not in this build: " + missing.join(" "));
    console.log("");
    console.log(pad("module", 20) + pad("names", 7) + pad("tested", 8) +
                pad("adversarial", 13) + "gaps");
    console.log("-".repeat(62));
    let T = 0, C = 0, A = 0;
    for (const r of [...byModule.values()].sort((a, b) => a.module < b.module ? -1 : 1)) {
        T += r.total; C += r.tested; A += r.adversarial;
        console.log(pad(r.module, 20) + pad(r.total, 7) + pad(r.tested, 8) +
                    pad(r.adversarial, 13) + (r.total - r.tested));
    }
    console.log("-".repeat(62));
    console.log(pad("TOTAL", 20) + pad(T, 7) + pad(C, 8) + pad(A, 13) + (T - C));
    console.log("\nadversarial coverage: " + A + "/" + T +
                " (" + (100 * A / T).toFixed(1) + "%)");
}
