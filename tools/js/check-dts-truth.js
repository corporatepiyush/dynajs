import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";
import { readText, writeText, pyRepr, pySplitlines, Out } from "./pyshim.js";

const DTS = "dynajs.d.ts";
const FAILURES = [];
const DTS_MUST_CONTAIN = [
 [
  "Negotiate returns the candidate string",
  "function Negotiate(header: string, candidates: string[]): string | null;"
 ],
 [
  "Object.pick runtime arg order is (keys, obj)",
  "pick(keys: string[], obj: object)"
 ],
 [
  "Object.pickAll runtime arg order is (keys, obj)",
  "pickAll(keys: string[], obj: object)"
 ],
 [
  "Object.omit runtime arg order is (keys, obj)",
  "omit(keys: string[], obj: object)"
 ],
 [
  "String.splitN separator is a plain string",
  "splitN(sep: string, n: number): string[];"
 ],
 [
  "Command.parse argv is optional (generic T)",
  "parse<T = { options: Record<string, unknown>; arguments: string[]; command: string | null }>(args?: string[]): T;"
 ],
 [
  "Number.range returns a real array",
  "range(start: number, end?: number, step?: number): number[];"
 ],
 [
  "dyna:net is declared in exactly one block",
  "declare module \"dyna:net\" {"
 ],
 [
  "queryIter is the NATIVE cursor driver on the class",
  "queryIter is NATIVE (a DECLARE CURSOR/FETCH FORWARD driver"
 ],
 [
  "queryIter's pool.js note matches the native reality",
  "src/pool.js no longer needs importing for this: its installer"
 ]
];
const DTS_MUST_CONTAIN_ONCE = [
 [
  "exactly one dyna:net module block",
  "declare module \"dyna:net\" {"
 ],
 [
  "queryIter declared exactly once",
  "queryIter(sql: string, params?: unknown[], opts?: { batch?: number; maxRows?: number }): AsyncIterableIterator<Record<string, unknown>>;"
 ]
];
const DTS_TRUTH_FIXES = [
 [
  "global fetch is dyna:http's by reference",
  "declare const fetch: typeof import(\"dyna:http\").fetch;"
 ],
 [
  "LRU.deserialize carries T through",
  "static deserialize<T>(this: new (capacity: number"
 ],
 [
  "SortedMap.deserialize carries T through",
  "static deserialize<T>(this: new () => SortedMap<T>"
 ],
 [
  "JWTVerify is generic",
  "function JWTVerify<T = unknown>(token: string, key: BytesInput"
 ],
 [
  "oauth2.verifyJWT is generic",
  "function verifyJWT<T = unknown>(token: string, key: BytesInput"
 ],
 [
  "Extractor.run takes the shared dyna:html tree",
  "run(doc: import(\"dyna:html\").HTMLElement | import(\"dyna:html\").HTMLElement[]"
 ],
 [
  "Crawl pages have a real interface",
  "interface CrawlPage {"
 ],
 [
  "ml Pipeline stages are typed",
  "type PipelineStage = LinearRegression"
 ],
 [
  "AbortSignal._abort is marked @internal",
  "/** @internal Fires the abort. Engine-internal mutator"
 ],
 [
  "String.lazy is a lazy-iterator method",
  "lazy(): Iterator<string>;"
 ],
 [
  "Array.lazy is a lazy-iterator method",
  "lazy(): Iterator<T>;"
 ],
 [
  "simd.cumsum overload follows the input (i32)",
  "function cumsum(a: Int32Array): Int32Array;"
 ],
 [
  "simd.cumsum overload follows the input (f32)",
  "function cumsum(a: Float32Array): Float32Array;"
 ],
 [
  "simd.cummax overload follows the input (i32)",
  "function cummax(a: Int32Array): Int32Array;"
 ],
 [
  "simd.cummax overload follows the input (f32)",
  "function cummax(a: Float32Array): Float32Array;"
 ],
 [
  "method-vs-getter mapping documented",
  "Method-vs-getter note"
 ],
 [
  "the casing/name-drift table lives in the header",
  "METHOD <-> FREE-FUNCTION MAP"
 ],
 [
  "the casing table names the drifted equal() pair",
  "Bytes.equals(v) <-> equal(a, b)"
 ],
 [
  "TextDecoder decode documents the streaming carry",
  "{stream: true} continues the decoder's stream"
 ],
 [
  "parseDuration says NANOSECONDS in caps",
  "of NANOSECONDS"
 ],
 [
  "parseDuration names the bigint threshold",
  "or above 2^53 ns a `bigint`"
 ],
 [
  "parseDuration excludes the 'd' unit",
  "\"d\" is NOT accepted"
 ],
 [
  "queueMicrotask is declared",
  "declare function queueMicrotask(callback: () => void): void;"
 ],
 [
  "mean is marked legacy of average",
  "LEGACY alias of average"
 ],
 [
  "uniq is marked legacy of unique",
  "LEGACY alias of unique"
 ],
 [
  "intersection is marked legacy of intersect",
  "LEGACY alias of intersect"
 ],
 [
  "merge is marked legacy of mergeRight",
  "LEGACY alias of mergeRight"
 ],
 [
  "invertObj is marked legacy of invert",
  "LEGACY alias of invert"
 ],
 [
  "Redis options declare tls",
  "/** Connect through TLS; certificates are verified against the system trust store"
 ],
 [
  "TO_CSV declares escapeFormulas",
  "TO_CSV(opts?: { escapeFormulas?: boolean }): string;"
 ],
 [
  "HTTPClient declares the onConnect hook",
  "onConnect?: (ip: string) => unknown;"
 ],
 [
  "DataFrame ctor accepts number[] columns",
  "Float64Array | number[] | string[] | { codes: Int32Array; values: string[] }>): DataFrame;"
 ],
 [
  "Lens is declared",
  "interface Lens<S = any, A = any> {"
 ],
 [
  "Float16Array is declared",
  "interface Float16ArrayConstructor {"
 ],
 [
  "DisposableStack is declared",
  "declare const DisposableStack: {"
 ],
 [
  "AsyncDisposableStack is declared",
  "declare const AsyncDisposableStack: {"
 ],
 [
  "SuppressedError is declared",
  "interface SuppressedError extends Error {"
 ],
 [
  "InternalError is declared",
  "declare const InternalError: {"
 ],
 [
  "__loadScript is declared @internal",
  "declare function __loadScript(filename: string): unknown;"
 ],
 [
  "Iterator statics are declared",
  "interface IteratorConstructor {"
 ],
 [
  "std/os compat matrix lives in the header",
  "`std`/`os`"
 ]
];
const BAG_KEY_MUTATIONS = [
 [
  "queryIter batch option",
  "batch?: number",
  "batchRenamed?: number"
 ],
 [
  "queryIter maxRows option",
  "maxRows?: number",
  "maxRowsRenamed?: number"
 ],
 [
  "TextDecoder stream option",
  "{stream: true}",
  "{streamRenamed: true}"
 ],
 [
  "TO_CSV escapeFormulas option",
  "escapeFormulas?: boolean",
  "escapeFormulasRenamed?: boolean"
 ],
 [
  "HTTPClient onConnect hook",
  "onConnect?:",
  "onConnectRenamed?:"
 ]
];
const PROBES = "\nimport { Negotiate } from \"dyna:http\";\nconst out = [];\nconst check = (name, ok) => out.push([name, !!ok]);\n\n// -- the original five truths --\ncheck(\"Negotiate-string\", Negotiate(\"text/html;q=1, text/plain;q=0.5\", [\"text/plain\",\"text/html\"]) === \"text/html\");\ncheck(\"pick-keys-obj\", JSON.stringify(Object.pick([\"a\"], { a: 1, b: 2 })) === '{\"a\":1}');\ncheck(\"splitN-string-sep\", \"a:b:c\".splitN(\":\", 2)[1] === \"b:c\");\ncheck(\"range-is-array\", Array.isArray(Number.range(1, 3)));\ncheck(\"Command-parse-optional\", (new (await import(\"dyna:cli\")).Command(\"t\")).parse() !== undefined);\n\n// -- runtime pins for the doc claims above --\nconst { parseDuration } = await import(\"dyna:time\");\ncheck(\"parseDuration-bigint\", typeof parseDuration(\"200000h\") === \"bigint\");\ncheck(\"parseDuration-ns\", parseDuration(\"300ms\") === 300000000);\nlet dayUnitThrew = false;\ntry { parseDuration(\"1d\"); } catch (e) { dayUnitThrew = true; }\ncheck(\"parseDuration-no-day-unit\", dayUnitThrew);\n\ncheck(\"queueMicrotask-global\", typeof queueMicrotask === \"function\");\ncheck(\"btoa-latin1\", btoa(\"\\u00e9\") === \"6Q==\");\n\nconst httpm = await import(\"dyna:http\");\ncheck(\"fetch-identity\", fetch === httpm.fetch);\n\nconst arrIt = [...[1, 2, 3].lazy().map(x => x * 2)];\ncheck(\"lazy-iterator-helpers\", JSON.stringify(arrIt) === \"[2,4,6]\");\n\nconst simd = await import(\"dyna:simd\");\ncheck(\"cumsum-overloads\",\n  simd.cumsum(new Int32Array([1,2,3])).constructor === Int32Array &&\n  simd.cumsum(new Float32Array([1,2,3])).constructor === Float32Array);\n\nconst { DataFrame } = await import(\"dyna:dataframe\");\nconst dfN = new DataFrame({ a: [1, 2], b: [\"x\", \"y\"] });\ncheck(\"dataframe-number-columns\", dfN.ROWS === 2);\nconst dfE = new DataFrame({ c: [\"=1+1\"] });\ncheck(\"to-csv-escapeFormulas\", dfE.TO_CSV({ escapeFormulas: true }).includes(\"'=1+1\"));\n\nconst { BTree, SortedMap } = await import(\"dyna:structures\");\nconst bt = new BTree(); bt.set(1, \"one\");\nlet mismatchThrew = false;\ntry { SortedMap.deserialize(bt.serialize()); } catch (e) { mismatchThrew = true; }\ncheck(\"serialize-formats-distinct\", mismatchThrew);\n\nconst dec = new TextDecoder();\nconst enc = new TextEncoder().encode(\"\\u00e9\");\nconst split = dec.decode(enc.slice(0, 1), { stream: true }) + dec.decode(enc.slice(1), { stream: true });\ncheck(\"decoder-stream-carries\", split === \"\\u00e9\");\n{\n    // a stream's carry is processed by the FINAL (no {stream:true}) call, and\n    // an incomplete tail left at final-call time becomes U+FFFD exactly once\n    const d2 = new TextDecoder();\n    check(\"decoder-mid-stream-silent\", d2.decode(new Uint8Array([0xC2]), { stream: true }) === \"\");\n    check(\"decoder-final-consumes-carry\", d2.decode(new Uint8Array([0xA0])) === \"\\u00a0\");\n    const d3 = new TextDecoder();\n    check(\"decoder-final-incomplete-replaced\", d3.decode(new Uint8Array([0xC3])) === \"\\uFFFD\");\n    check(\"decoder-state-resets\", d3.decode(new Uint8Array([0x61])) === \"a\");\n}\n\nconst { PostgreSQL } = await import(\"dyna:postgres\");\ncheck(\"queryIter-native\", typeof PostgreSQL.prototype.queryIter === \"function\");\n\ncheck(\"iterator-helpers\", JSON.stringify(Iterator.from([1, 2]).map(x => x + 1).toArray()) === \"[2,3]\");\ncheck(\"lens-laws\", Lens.prop(\"a\").set(9, { a: 1 }).a === 9 && Lens.path(\"x.y\").view({ x: { y: 5 } }) === 5);\ncheck(\"float16array-bytes\", Float16Array.BYTES_PER_ELEMENT === 2);\n\nlet bad = 0;\nfor (const [n, ok] of out) if (!ok) { bad++; console.log(\"RUNTIME-DRIFT: \" + n); }\nconsole.log(\"runtime-probes: \" + (out.length - bad) + \"/\" + out.length);\nif (bad) throw new Error(\"drift\");\n";

const count = (s, n) => s.split(n).length - 1;

function needleFailures(src) {
    const fails = [];
    for (const [what, needle] of DTS_MUST_CONTAIN) if (!src.includes(needle)) fails.push(`d.ts: ${what} -- missing ${pyRepr(needle)}`);
    for (const [what, needle] of DTS_MUST_CONTAIN_ONCE) {
        const n = count(src, needle);
        if (n !== 1) fails.push(`d.ts: ${what} -- found ${n} times, want exactly 1`);
    }
    for (const [what, needle] of DTS_TRUTH_FIXES) if (!src.includes(needle)) fails.push(`d.ts: ${what} -- missing ${pyRepr(needle)}`);
    return fails;
}

function selftest() {
    const src = readText(DTS);
    const out = [];
    const base = needleFailures(src);
    if (base.length) {
        out.push(`selftest: the UNMUTATED d.ts is already red -- fix the needles or the d.ts first (${base.length} failures)`);
        return out;
    }
    for (const [what, old, nw] of BAG_KEY_MUTATIONS) {
        if (!src.includes(old)) {
            out.push(`selftest: ${what} -- mutation target ${pyRepr(old)} is not in the d.ts; the key is no longer declared as pinned`);
            continue;
        }
        if (!needleFailures(src.replaceAll(old, nw)).length)
            out.push(`selftest: ${what} -- renaming ${pyRepr(old)} did NOT redden the needles; the bag key is unbound`);
    }
    return out;
}

function checkDts() {
    FAILURES.push(...needleFailures(readText(DTS)));
    FAILURES.push(...selftest());
}

function checkRuntime(dynajs) {
    const tmp = (std.getenv("TMPDIR") || "/tmp/").replace(/\/?$/, "/");
    const path = `${tmp}tmp${os.getpid()}_${Date.now()}.mjs`;
    writeText(path, PROBES);
    try {
        let p;
        try {
            p = Exec(dynajs, [path], { timeoutMs: 30000, env: std.getenviron() });
        } catch (e) {
            FAILURES.push("runtime probes failed: " + e.message);
            return;
        }
        if (p.code !== 0) {
            FAILURES.push(`runtime probes failed: ${p.stdout.slice(-300)}${p.stderr.slice(-300)}`);
            return;
        }
        const combined = p.stdout + p.stderr;
        for (const line of pySplitlines(combined))
            if (line.startsWith("RUNTIME-DRIFT:")) FAILURES.push("runtime: " + line);
        if (!combined.includes("runtime-probes: ")) FAILURES.push(`runtime probes produced no summary: ${pyRepr(combined.slice(-200))}`);
    } finally {
        os.remove(path);
    }
}

function main() {
    const o = new Out();
    let dynajs = "./dynajs", onlySelftest = false;
    const args = scriptArgs.slice(1);
    for (let i = 0; i < args.length; ) {
        if (args[i] === "--dynajs" && i + 1 < args.length) {
            dynajs = args[i + 1];
            i += 2;
        } else if (args[i] === "--selftest") {
            onlySelftest = true;
            i++;
        } else {
            o.print("usage: check-dts-truth.py [--dynajs PATH] [--selftest]");
            o.flush();
            return 2;
        }
    }
    if (onlySelftest) {
        const st = selftest();
        for (const f of st) o.print("FAIL " + f);
        if (!st.length) o.print(`check-dts-truth selftest: every rename-a-bag-key mutation reddens the needles (${BAG_KEY_MUTATIONS.length} mutations)`);
        o.flush();
        return st.length ? 1 : 0;
    }
    checkDts();
    checkRuntime(dynajs);
    const nText = DTS_MUST_CONTAIN.length + DTS_MUST_CONTAIN_ONCE.length + DTS_TRUTH_FIXES.length;
    if (FAILURES.length) {
        for (const f of FAILURES) o.print("FAIL " + f);
        o.flush();
        return 1;
    }
    o.print(`check-dts-truth: ${nText} signatures + runtime probes + ${BAG_KEY_MUTATIONS.length} rename-a-bag-key mutations all hold`);
    o.flush();
    return 0;
}
std.exit(main());
