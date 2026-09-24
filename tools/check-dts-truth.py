#!/usr/bin/env python3
"""tools/check-dts-truth.py -- d.ts truth gate: needles + runtime probes.

SCOPE, stated exactly: the d.ts needle half is PRESENCE-ONLY. Each needle
pins that a documented truth EXISTS in dynajs.d.ts; it cannot see a STALE
claim added elsewhere in the file (an extra wrong comment still passes), and
it does not check EXPORT drift (declared-but-not-exported and friends) --
that is the companion tool's job (tools/check-dts-coverage.py). What the
needles DO bind is content: every needle is text present in the CURRENT
declaration file on the merge target, with no tag or ticket prefixes, and the
option-bag key names inside them are bound tightly enough that renaming one
is caught (see BAG_KEY_MUTATIONS and --selftest below).

Locks the dynajs.d.ts signatures that were once WRONG to the
RUNTIME's actual behaviour, using two independent oracles:

  1. d.ts text assertions -- the declarations must say what the binary does.
  2. runtime probes       -- ./dynajs executes each contested call the way a
     user transpiled from the d.ts would write it.

Run: python3 tools/check-dts-truth.py [--dynajs PATH] [--selftest]
Exit 0 = truths hold; nonzero = drift (fix the d.ts, or fix the runtime).
--selftest additionally proves the needle set binds what it claims: every
rename-a-bag-key mutation below must redden the needles (a mutation the
needles do NOT catch means a bag key silently unbound itself).
"""
import subprocess
import sys

DTS = "dynajs.d.ts"
FAILURES = []

# ---- oracle 1: d.ts text must match runtime truth ------------------------
DTS_MUST_CONTAIN = [
    # (what, needle) -- each needle is the CORRECT signature fragment
    ("Negotiate returns the candidate string",
     "function Negotiate(header: string, candidates: string[]): string | null;"),
    ("Object.pick runtime arg order is (keys, obj)",
     "pick(keys: string[], obj: object)"),
    ("Object.pickAll runtime arg order is (keys, obj)",
     "pickAll(keys: string[], obj: object)"),
    ("Object.omit runtime arg order is (keys, obj)",
     "omit(keys: string[], obj: object)"),
    ("String.splitN separator is a plain string",
     "splitN(sep: string, n: number): string[];"),
    ("Command.parse argv is optional (generic T)",
     "parse<T = { options: Record<string, unknown>; arguments: string[]; command: string | null }>(args?: string[]): T;"),
    ("Number.range returns a real array",
     "range(start: number, end?: number, step?: number): number[];"),

    # ---- ONE dyna:net block; queryIter declared on the class ----
    ("dyna:net is declared in exactly one block",
     'declare module "dyna:net" {'),
    ("queryIter is the NATIVE cursor driver on the class",
     "queryIter is NATIVE (a DECLARE CURSOR/FETCH FORWARD driver"),
    ("queryIter's pool.js note matches the native reality",
     "src/pool.js no longer needs importing for this: its installer"),
]
DTS_MUST_CONTAIN_ONCE = [
    # (what, needle) -- exactly one occurrence (duplicate-block regression guard)
    ("exactly one dyna:net module block",
     'declare module "dyna:net" {'),
    ("queryIter declared exactly once",
     "queryIter(sql: string, params?: unknown[], opts?: { batch?: number; maxRows?: number }): AsyncIterableIterator<Record<string, unknown>>;"),
]

# ---- assertion table (what, needle) -------------------------------------
# Every needle is CONTENT: plain declaration text present in the CURRENT
# dynajs.d.ts on the merge target. No tag or ticket prefixes -- a needle that
# matches only a marked-up copy of the file is a needle that fails on the
# tree the gate has to protect.
DTS_TRUTH_FIXES = [
    ("global fetch is dyna:http's by reference",
     "declare const fetch: typeof import(\"dyna:http\").fetch;"),
    ("LRU.deserialize carries T through",
     "static deserialize<T>(this: new (capacity: number"),
    ("SortedMap.deserialize carries T through",
     "static deserialize<T>(this: new () => SortedMap<T>"),
    ("JWTVerify is generic",
     "function JWTVerify<T = unknown>(token: string, key: BytesInput"),
    ("oauth2.verifyJWT is generic",
     "function verifyJWT<T = unknown>(token: string, key: BytesInput"),
    ("Extractor.run takes the shared dyna:html tree",
     "run(doc: import(\"dyna:html\").HTMLElement | import(\"dyna:html\").HTMLElement[]"),
    ("Crawl pages have a real interface",
     "interface CrawlPage {"),
    ("ml Pipeline stages are typed",
     "type PipelineStage = LinearRegression"),
    ("AbortSignal._abort is marked @internal",
     "/** @internal Fires the abort. Engine-internal mutator"),
    ("String.lazy is a lazy-iterator method",
     "lazy(): Iterator<string>;"),
    ("Array.lazy is a lazy-iterator method",
     "lazy(): Iterator<T>;"),
    ("simd.cumsum overload follows the input (i32)",
     "function cumsum(a: Int32Array): Int32Array;"),
    ("simd.cumsum overload follows the input (f32)",
     "function cumsum(a: Float32Array): Float32Array;"),
    ("simd.cummax overload follows the input (i32)",
     "function cummax(a: Int32Array): Int32Array;"),
    ("simd.cummax overload follows the input (f32)",
     "function cummax(a: Float32Array): Float32Array;"),
    ("method-vs-getter mapping documented",
     "Method-vs-getter note"),
    ("the casing/name-drift table lives in the header",
     "METHOD <-> FREE-FUNCTION MAP"),
    ("the casing table names the drifted equal() pair",
     "method Bytes.equals(v)   <-> free equal(a, b)"),
    ("TextDecoder decode documents the streaming carry",
     "{stream: true} continues the decoder's stream"),
    ("parseDuration says NANOSECONDS in caps",
     "of NANOSECONDS"),
    ("parseDuration names the bigint threshold",
     "or above 2^53 ns a `bigint`"),
    ("parseDuration excludes the 'd' unit",
     "\"d\" is NOT accepted"),
    ("queueMicrotask is declared",
     "declare function queueMicrotask(callback: () => void): void;"),
    ("mean is marked legacy of average",
     "LEGACY alias of average"),
    ("uniq is marked legacy of unique",
     "LEGACY alias of unique"),
    ("intersection is marked legacy of intersect",
     "LEGACY alias of intersect"),
    ("merge is marked legacy of mergeRight",
     "LEGACY alias of mergeRight"),
    ("invertObj is marked legacy of invert",
     "LEGACY alias of invert"),
    ("Redis options declare tls",
     "/** Connect through TLS (the handshake runs on the aio engine). */"),
    ("TO_CSV declares escapeFormulas",
     "TO_CSV(opts?: { escapeFormulas?: boolean }): string;"),
    ("HTTPClient declares the onConnect hook",
     "onConnect?: (ip: string) => unknown;"),
    ("DataFrame ctor accepts number[] columns",
     "Float64Array | number[] | string[]>): DataFrame;"),
    ("Lens is declared",
     "interface Lens<S = any, A = any> {"),
    ("Float16Array is declared",
     "interface Float16ArrayConstructor {"),
    ("DisposableStack is declared",
     "declare const DisposableStack: {"),
    ("AsyncDisposableStack is declared",
     "declare const AsyncDisposableStack: {"),
    ("SuppressedError is declared",
     "interface SuppressedError extends Error {"),
    ("InternalError is declared",
     "declare const InternalError: {"),
    ("__loadScript is declared @internal",
     "declare function __loadScript(filename: string): unknown;"),
    ("Iterator statics are declared",
     "interface IteratorConstructor {"),
    ("std/os compat matrix lives in the header",
     "`std`/`os`"),
]

# ---- rename-a-bag-key mutation check (driven by --selftest) ---------------
# Each entry renames ONE option-bag key declaration in a copy of the d.ts
# text. The needle set must go RED for every one of them: if a mutation
# passes, the named key is no longer bound by any needle and can drift
# silently. These are exactly the bag keys the needles above embed.
BAG_KEY_MUTATIONS = [
    ("queryIter batch option", "batch?: number", "batchRenamed?: number"),
    ("queryIter maxRows option", "maxRows?: number", "maxRowsRenamed?: number"),
    ("TextDecoder stream option", "{stream: true}", "{streamRenamed: true}"),
    ("TO_CSV escapeFormulas option",
     "escapeFormulas?: boolean", "escapeFormulasRenamed?: boolean"),
    ("HTTPClient onConnect hook", "onConnect?:", "onConnectRenamed?:"),
]

# ---- oracle 2: the runtime must actually behave that way -----------------
PROBES = r"""
import { Negotiate } from "dyna:http";
const out = [];
const check = (name, ok) => out.push([name, !!ok]);

// -- the original five truths --
check("Negotiate-string", Negotiate("text/html;q=1, text/plain;q=0.5", ["text/plain","text/html"]) === "text/html");
check("pick-keys-obj", JSON.stringify(Object.pick(["a"], { a: 1, b: 2 })) === '{"a":1}');
check("splitN-string-sep", "a:b:c".splitN(":", 2)[1] === "b:c");
check("range-is-array", Array.isArray(Number.range(1, 3)));
check("Command-parse-optional", (new (await import("dyna:cli")).Command("t")).parse() !== undefined);

// -- runtime pins for the doc claims above --
const { parseDuration } = await import("dyna:time");
check("parseDuration-bigint", typeof parseDuration("200000h") === "bigint");
check("parseDuration-ns", parseDuration("300ms") === 300000000);
let dayUnitThrew = false;
try { parseDuration("1d"); } catch (e) { dayUnitThrew = true; }
check("parseDuration-no-day-unit", dayUnitThrew);

check("queueMicrotask-global", typeof queueMicrotask === "function");
check("btoa-latin1", btoa("\u00e9") === "6Q==");

const httpm = await import("dyna:http");
check("fetch-identity", fetch === httpm.fetch);

const arrIt = [...[1, 2, 3].lazy().map(x => x * 2)];
check("lazy-iterator-helpers", JSON.stringify(arrIt) === "[2,4,6]");

const simd = await import("dyna:simd");
check("cumsum-overloads",
  simd.cumsum(new Int32Array([1,2,3])).constructor === Int32Array &&
  simd.cumsum(new Float32Array([1,2,3])).constructor === Float32Array);

const { DataFrame } = await import("dyna:dataframe");
const dfN = new DataFrame({ a: [1, 2], b: ["x", "y"] });
check("dataframe-number-columns", dfN.ROWS === 2);
const dfE = new DataFrame({ c: ["=1+1"] });
check("to-csv-escapeFormulas", dfE.TO_CSV({ escapeFormulas: true }).includes("'=1+1"));

const { BTree, SortedMap } = await import("dyna:structures");
const bt = new BTree(); bt.set(1, "one");
let mismatchThrew = false;
try { SortedMap.deserialize(bt.serialize()); } catch (e) { mismatchThrew = true; }
check("serialize-formats-distinct", mismatchThrew);

const dec = new TextDecoder();
const enc = new TextEncoder().encode("\u00e9");
const split = dec.decode(enc.slice(0, 1), { stream: true }) + dec.decode(enc.slice(1), { stream: true });
check("decoder-stream-carries", split === "\u00e9");
{
    // a stream's carry is processed by the FINAL (no {stream:true}) call, and
    // an incomplete tail left at final-call time becomes U+FFFD exactly once
    const d2 = new TextDecoder();
    check("decoder-mid-stream-silent", d2.decode(new Uint8Array([0xC2]), { stream: true }) === "");
    check("decoder-final-consumes-carry", d2.decode(new Uint8Array([0xA0])) === "\u00a0");
    const d3 = new TextDecoder();
    check("decoder-final-incomplete-replaced", d3.decode(new Uint8Array([0xC3])) === "\uFFFD");
    check("decoder-state-resets", d3.decode(new Uint8Array([0x61])) === "a");
}

const { PostgreSQL } = await import("dyna:net");
check("queryIter-native", typeof PostgreSQL.prototype.queryIter === "function");

check("iterator-helpers", JSON.stringify(Iterator.from([1, 2]).map(x => x + 1).toArray()) === "[2,3]");
check("lens-laws", Lens.prop("a").set(9, { a: 1 }).a === 9 && Lens.path("x.y").view({ x: { y: 5 } }) === 5);
check("float16array-bytes", Float16Array.BYTES_PER_ELEMENT === 2);

let bad = 0;
for (const [n, ok] of out) if (!ok) { bad++; console.log("RUNTIME-DRIFT: " + n); }
console.log("runtime-probes: " + (out.length - bad) + "/" + out.length);
if (bad) throw new Error("drift");
"""


def needle_failures(src):
    """Every needle check against one d.ts TEXT. Presence-only: see the
    header for what that does and does not cover."""
    fails = []
    for what, needle in DTS_MUST_CONTAIN:
        if needle not in src:
            fails.append("d.ts: %s -- missing %r" % (what, needle))
    for what, needle in DTS_MUST_CONTAIN_ONCE:
        n = src.count(needle)
        if n != 1:
            fails.append("d.ts: %s -- found %d times, want exactly 1"
                         % (what, n))
    for what, needle in DTS_TRUTH_FIXES:
        if needle not in src:
            fails.append("d.ts: %s -- missing %r" % (what, needle))
    return fails


def selftest():
    """Rename-a-bag-key mutation check: every option-bag key the needles pin
    must be bound tightly enough that renaming its declaration REDDENS the
    needle set. A mutation that sails through means that key can drift with
    no gate noticing -- add a needle that binds it."""
    src = open(DTS, encoding="utf-8", errors="replace").read()
    out = []
    base = needle_failures(src)
    if base:
        out.append("selftest: the UNMUTATED d.ts is already red -- fix the "
                   "needles or the d.ts first (%d failures)" % len(base))
        return out
    for what, old, new in BAG_KEY_MUTATIONS:
        if old not in src:
            out.append("selftest: %s -- mutation target %r is not in the "
                       "d.ts; the key is no longer declared as pinned"
                       % (what, old))
            continue
        mutated = src.replace(old, new)
        if not needle_failures(mutated):
            out.append("selftest: %s -- renaming %r did NOT redden the "
                       "needles; the bag key is unbound" % (what, old))
    return out


def check_dts():
    src = open(DTS, encoding="utf-8", errors="replace").read()
    FAILURES.extend(needle_failures(src))
    FAILURES.extend(selftest())


def check_runtime(dynajs):
    import tempfile, os
    fd, path = tempfile.mkstemp(suffix=".mjs")
    with os.fdopen(fd, "w") as fh:
        fh.write(PROBES)
    try:
        p = subprocess.run([dynajs, path], capture_output=True, text=True,
                           timeout=30)
        if p.returncode != 0:
            FAILURES.append("runtime probes failed: %s%s"
                            % (p.stdout[-300:], p.stderr[-300:]))
            return
        combined = p.stdout + p.stderr
        for line in combined.splitlines():
            if line.startswith("RUNTIME-DRIFT:"):
                FAILURES.append("runtime: " + line)
        if "runtime-probes: " not in combined:
            FAILURES.append("runtime probes produced no summary: %r"
                            % (combined[-200:],))
    finally:
        os.unlink(path)


def main():
    dynajs = "./dynajs"
    only_selftest = False
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--dynajs" and i + 1 < len(args):
            dynajs = args[i + 1]
            i += 2
        elif args[i] == "--selftest":
            only_selftest = True
            i += 1
        else:
            print("usage: check-dts-truth.py [--dynajs PATH] [--selftest]")
            return 2
    if only_selftest:
        st = selftest()
        for f in st:
            print("FAIL " + f)
        if not st:
            print("check-dts-truth selftest: every rename-a-bag-key "
                  "mutation reddens the needles (%d mutations)"
                  % len(BAG_KEY_MUTATIONS))
        return 1 if st else 0
    check_dts()
    check_runtime(dynajs)
    n_text = len(DTS_MUST_CONTAIN) + len(DTS_MUST_CONTAIN_ONCE) + len(DTS_TRUTH_FIXES)
    if FAILURES:
        for f in FAILURES:
            print("FAIL " + f)
        return 1
    print("check-dts-truth: %d signatures + runtime probes + %d rename-a-"
          "bag-key mutations all hold" % (n_text, len(BAG_KEY_MUTATIONS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
