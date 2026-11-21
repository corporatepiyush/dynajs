// tools/dod_scan.js -- data-oriented-design scan of a C code base: every loop, struct, allocation and hot-path
// idiom, joined with what the compiler said and (optionally) where a sampling profile says the time goes.
//
// Generic by default: works on any C tree. Project knowledge is DATA, in dod_scan.json (or tools/dod_scan.json)
// at the root -- extra allocator/free/cold/push names (exact names, or "re:<regex>"), managed-allocation prefixes,
// API prefixes, type sizes, source roots, a compile-command hook, hardening flags (a fixed list, a cache glob,
// or a probe recipe), profile workloads and the binary to run them with. Without it the built-in generic
// patterns apply and compile commands come from compile_commands.json, else from a plain `cc -O2`.
//
// Compiler sources (the oracle for these columns; nothing here guesses them): loop-vectorize, licm, gvn,
// regalloc, slp-vectorizer and inline optimization records, -Wpadded, and the complete record layouts
// (exact struct sizes and field offsets). Profile source: macOS `sample` + llvm-symbolizer (inlined frames
// expanded), giving self-time per source line, so each finding carries the share of time spent inside it.
//
// The lexical checks follow sections of the "Extreme Performance Engineering Reference" (gist
// corporatepiyush/45b22003498f43b7a1901a1cc861d7a7) plus pointer-idiom checks; CHECKS lists each with its section.
//
// usage:
//   dynajs --std tools/dod_scan.js remarks   [--root DIR] [--project FILE] [--jobs N]   # compiler records + layouts
//   dynajs --std tools/dod_scan.js profile   [--binary BIN] [--seconds S]               # sample the configured workloads
//   dynajs --std tools/dod_scan.js aggregate                                            # re-symbolize saved samples
//   dynajs --std tools/dod_scan.js report    [--limit N] [--check NAME]                 # join + ranked tables
//   dynajs --std tools/dod_scan.js all | selftest                                       # selftest: every check fires on its fixture
// Regexes are written in JS syntax; the scan's lexical semantics (whitespace splitting, stable sorts, Python-style
// number formatting in the report) are kept identical to the original Python tool so outputs diff clean.
import * as std from "std";
import * as file from "dyna:file";
import * as sys from "dyna:sys";
import { Command } from "dyna:cli";
import { Multiset } from "dyna:structures";
import { Graph } from "dyna:graph";
import { f64Sum } from "dyna:simd";
import { pmap } from "dyna:async";
import { BLAKE3Hex } from "dyna:hash";
import { zstd, unzstd } from "dyna:compress";
import { MultiMatcher } from "dyna:matcher";
const { Path } = file;
const P = (p) => new Path(p);

const RXC = new Map();
const rxCached = (kind, source, flags, make) => {
    const k = kind + flags + "\x00" + source;
    let r = RXC.get(k);
    if (r === undefined) { r = make(); RXC.set(k, r); }
    return r;
};
const gre = (re) => rxCached("g", re.source, re.flags, () => new RegExp(re.source, re.flags.includes("g") ? re.flags : re.flags + "g"));
const flist = (re, s) => {
    const r = gre(re), out = [];
    r.lastIndex = 0;
    let m;
    while ((m = r.exec(s)) !== null) { out.push(m); if (m[0] === "") r.lastIndex++; }
    return out;
};
const fiter = flist;
const RX = (source, flags = "") => ({ source, flags });
const findall = (re, s) => flist(re, s).map(m => m.length === 1 ? m[0] : m.length === 2 ? m[1] : m.slice(1));
const search = (re, s) => { const r = rxCached("s", re.source, re.flags, () => new RegExp(re.source, re.flags.replace("g", ""))); return r.exec(s); };
const matchAt = (re, s, pos = 0) => {
    const y = rxCached("y", re.source, re.flags, () => new RegExp(re.source, re.flags.replace("g", "") + "y"));
    y.lastIndex = pos; return y.exec(s);
};
const fullmatch = (re, s) => { const f = rxCached("f", re.source, re.flags, () => new RegExp("(?:" + re.source + ")$", re.flags.replace("g", "") + "y")); f.lastIndex = 0; return f.exec(s); };
const mEnd = (m) => m.index + m[0].length;
const g = (m, i) => (m[i] === undefined ? null : m[i]);
const WS_ANY = /[ \t\n\r\f\v]/, WS_IRREG = /[\t\n\r\f\v]|  |^ | $/;
const words = (s) => (WS_ANY.test(s) ? s.split(/[ \t\n\r\f\v]+/).filter(x => x !== "") : s === "" ? [] : [s]);
const norm = (s) => (WS_IRREG.test(s) ? words(s).join(" ") : s);
const uniqSorted = (a) => [...new Set(a)].sort();
const reEscape = (s) => s.replace(/[.*+?^${}()|[\]\\\/-]/g, "\\$&");
const isDigits = (s) => /^[0-9]+$/.test(s);
const pyStrip = (s, chars) => { let a = 0, b = s.length; while (a < b && chars.includes(s[a])) a++; while (b > a && chars.includes(s[b - 1])) b--; return s.slice(a, b); };
const PY_WS = new Set([9, 10, 11, 12, 13, 28, 29, 30, 31, 32, 0x85, 0xa0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000]);
const wsStart = (s) => { let a = 0; while (a < s.length && PY_WS.has(s.charCodeAt(a))) a++; return a; };
const wsEnd = (s, a = 0) => { let b = s.length; while (b > a && PY_WS.has(s.charCodeAt(b - 1))) b--; return b; };
const pyTrim = (s) => { const a = wsStart(s); return s.slice(a, wsEnd(s, a)); };
const rstripWs = (s) => s.slice(0, wsEnd(s));
const lstripWs = (s) => s.slice(wsStart(s));
const rfind = (s, sub, start = 0, end = s.length) => { const from = end - sub.length; if (from < 0) return -1; const i = s.lastIndexOf(sub, from); return i >= start ? i : -1; };
const split1 = (s, sep) => { const i = s.indexOf(sep); return i < 0 ? [s] : [s.slice(0, i), s.slice(i + sep.length)]; };
const counter = () => new Multiset();
const cinc = (c, k, n = 1) => c.add(k, n);
const mostCommon = (c) => [...c.entries()].sort((a, b) => b[1] - a[1]);
const sortedBy = (a, key) => a.map((v, i) => [key(v), i, v]).sort((x, y) => cmp(x[0], y[0]) || x[1] - y[1]).map(x => x[2]);
const cmp = (a, b) => {
    if (Array.isArray(a)) { for (let i = 0; i < Math.min(a.length, b.length); i++) { const c = cmp(a[i], b[i]); if (c) return c; } return a.length - b.length; }
    return a < b ? -1 : a > b ? 1 : 0;
};
const exists = (p) => file.exists(P(p));
const statOf = (p) => { try { return file.stat(P(p)); } catch (e) { return null; } };
const isDir = (p) => { const s = statOf(p); return !!s && !!s.isDir; };
const readText = (p) => { try { return file.readFile(P(p)).replace(/\r\n?/g, "\n"); } catch (e) { return null; } };
const writeText = (p, t) => file.writeFile(P(p), t);
const listDir = (p) => file.readDir(P(p)).map(e => e.name).sort();
const abspath = (p) => { if (!p.startsWith("/")) p = sys.cwd() + "/" + p; const out = []; for (const x of p.split("/")) { if (x === "" || x === ".") continue; if (x === "..") out.pop(); else out.push(x); } return "/" + out.join("/"); };
const dirname = (p) => { const i = p.lastIndexOf("/"); return i < 0 ? "" : i === 0 ? "/" : p.slice(0, i); };
const basename = (p) => p.slice(p.lastIndexOf("/") + 1);
const join = (...a) => a.reduce((x, y) => (y.startsWith("/") ? y : x === "" ? y : x.endsWith("/") ? x + y : x + "/" + y), "");
const relpath = (p, base) => {
    const a = abspath(p).split("/").filter(Boolean), b = abspath(base).split("/").filter(Boolean);
    let i = 0; while (i < a.length && i < b.length && a[i] === b[i]) i++;
    const r = [...b.slice(i).map(() => ".."), ...a.slice(i)].join("/");
    return r || ".";
};
const makedirs = (p) => file.makeDir(P(p), { recursive: true });
const walk = (top, cb) => {
    let ents;
    try { ents = file.readDir(P(top)); } catch (e) { return; }
    ents.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    const dirs = ents.filter(e => e.isDir && !e.isSymlink).map(e => e.name), files = ents.filter(e => !(e.isDir && !e.isSymlink)).map(e => e.name);
    cb(top, dirs, files);
    for (const d of dirs) walk(join(top, d), cb);
};

function shlexSplit(s) {
    const out = []; let cur = null, i = 0;
    while (i < s.length) {
        const c = s[i];
        if (/\s/.test(c)) { if (cur !== null) { out.push(cur); cur = null; } i++; continue; }
        if (cur === null) cur = "";
        if (c === "'") { const j = s.indexOf("'", i + 1); if (j < 0) throw new Error("No closing quotation"); cur += s.slice(i + 1, j); i = j + 1; }
        else if (c === '"') {
            i++;
            while (i < s.length && s[i] !== '"') { if (s[i] === "\\" && i + 1 < s.length && '\\"$`\n'.includes(s[i + 1])) { cur += s[i + 1]; i += 2; } else cur += s[i++]; }
            if (i >= s.length) throw new Error("No closing quotation");
            i++;
        } else if (c === "\\") { if (i + 1 < s.length) cur += s[i + 1]; i += 2; }
        else { cur += c; i++; }
    }
    if (cur !== null) out.push(cur);
    return out;
}
const shQuote = (s) => (s !== "" && /^[\w@%+=:,./-]+$/.test(s) ? s : "'" + s.replace(/'/g, "'\"'\"'") + "'");

function pyEval(expr) {
    const toks = []; let i = 0;
    while (i < expr.length) {
        const c = expr[i];
        if (/\s/.test(c)) { i++; continue; }
        if (/\d/.test(c)) { let j = i; while (j < expr.length && /\d/.test(expr[j])) j++; const t = expr.slice(i, j); if (t.length > 1 && t[0] === "0" && /[1-9]/.test(t)) return null; toks.push({ n: BigInt(t) }); i = j; continue; }
        const two = expr.slice(i, i + 2);
        if (two === "**" || two === "//" || two === "<<") { toks.push(two); i += 2; continue; }
        toks.push(c); i++;
    }
    let p = 0;
    const isF = (v) => typeof v === "number";
    const num = (v) => (isF(v) ? v : Number(v));
    const arith = (op, a, b) => {
        if (op === "/") { const d = num(b); if (d === 0) throw 0; return num(a) / d; }
        if (op === "//") {
            if (isF(a) || isF(b)) { const d = num(b); if (d === 0) throw 0; return Math.floor(num(a) / d); }
            if (b === 0n) throw 0; let q = a / b; if ((a % b !== 0n) && ((a < 0n) !== (b < 0n))) q -= 1n; return q;
        }
        if (op === "**") {
            if (isF(a) || isF(b) || b < 0n) return Math.pow(num(a), num(b));
            if (b > 100000n) throw 0; return a ** b;
        }
        if (op === "<<") { if (isF(a) || isF(b)) throw 0; if (b < 0n) throw 0; if (b > 100000n) throw 0; return a << b; }
        if (isF(a) || isF(b)) return op === "+" ? num(a) + num(b) : op === "-" ? num(a) - num(b) : num(a) * num(b);
        return op === "+" ? a + b : op === "-" ? a - b : a * b;
    };
    const atom = () => {
        const t = toks[p];
        if (t === "(") { p++; const v = cmpx(); if (toks[p] !== ")") throw 0; p++; return v; }
        if (t && t.n !== undefined) { p++; return t.n; }
        throw 0;
    };
    const unary = () => {
        if (toks[p] === "-" || toks[p] === "+") { const op = toks[p++]; const v = unary(); return op === "+" ? v : isF(v) ? -v : -v; }
        return power();
    };
    const power = () => { const a = atom(); if (toks[p] === "**") { p++; return arith("**", a, unary()); } return a; };
    const term = () => { let v = unary(); while (["*", "/", "//"].includes(toks[p])) { const op = toks[p++]; v = arith(op, v, unary()); } return v; };
    const sum = () => { let v = term(); while (toks[p] === "+" || toks[p] === "-") { const op = toks[p++]; v = arith(op, v, term()); } return v; };
    const shift = () => { let v = sum(); while (toks[p] === "<<") { p++; v = arith("<<", v, sum()); } return v; };
    const cmpx = () => {
        let v = shift();
        if (toks[p] !== "<") return v;
        let res = true;
        while (toks[p] === "<") { p++; const w = shift(); res = res && num(v) < num(w) && (isF(v) || isF(w) || v < w); v = w; }
        return res ? 1n : 0n;
    };
    try {
        const v = cmpx();
        if (p !== toks.length) return null;
        if (isF(v)) { if (!Number.isFinite(v)) return null; return Math.trunc(v); }
        return Number(v);
    } catch (e) { return null; }
}

function findRoot(start) {
    for (const markers of [[".git", "compile_commands.json", "tools/dod_scan.json"], ["dod_scan.json"]]) {
        let d = abspath(start);
        while (d !== "/") {
            if (markers.some(m => exists(join(d, m)))) return d;
            d = dirname(d);
        }
    }
    return abspath(start);
}

const SCRIPT = abspath(scriptArgs[0]);
let ROOT = findRoot(dirname(SCRIPT));
let PROJECT = {};
let DEFAULT_OUT = join(ROOT, ".dod-scan");
const REMARK_PASSES = "loop-vectorize|licm|gvn|regalloc|slp-vectorizer|inline";

const CHECKS = [
    ["alloc-churn", "5.1/5.2/5.3", "loop allocates AND frees in its body: per-iteration churn; reuse scratch or an arena"],
    ["alloc-in-loop", "4.3/5.1", "allocation inside an innermost loop: pre-size once (reserve) or pool"],
    ["push-per-element", "3.2/5.1", "per-element push/append inside an innermost loop: reserve the total once"],
    ["smallcpy-blocks-vec", "4.4.5/4.8.8", "fixed-size memcpy/memset stays a CALL in a loop and blocks vectorization"],
    ["libm-in-loop", "4.4.5", "libm call in a non-vectorized innermost loop: hoist, or use a vectorizable form"],
    ["indirect-call-in-loop", "4.8.10/10.3", "indirect call in a small innermost loop: no inlining, branch-target prediction"],
    ["bound-reload", "4.8.1", "loop bound read through a pointer while the body stores or calls: reloaded every iteration"],
    ["call-in-bound", "4.8.1", "function call in the loop condition (strlen and friends): evaluated every iteration"],
    ["invariant-branch", "4.8.2/4.4.4", "if() on values the loop never changes: unswitch (two clean loops)"],
    ["cold-in-loop", "4.8.3", "error/throw/log call inside a small innermost loop without unlikely(): cold code in the hot block"],
    ["modulo-in-loop", "4.8.5", "% by a variable or a non-power-of-two constant in a loop: a division per iteration"],
    ["div-in-loop", "4.8.5", "division by a variable in an innermost loop: hoist a reciprocal or shift where exact"],
    ["non-unit-stride", "4.8.8", "array indexed with the induction variable times a stride: packed loads impossible"],
    ["accum-through-pointer", "4.8.6", "compound update of a heap field inside a loop: shadow into a local, write back once"],
    ["deep-chain", "4.8.7", "store through a 2+ hop pointer chain inside a loop: pull the leaf into a local"],
    ["transform-and-reduce", "4.8.8", "loop both stores an array and reduces into a scalar: split into two passes"],
    ["loop-fission", "4.8.8/4.8.9", "innermost loop whose statements split (by dependence SCCs) into a clean vectorizable piece and a piece that blocks it: two loops; assumes distinct arrays do not alias and callees touch only their arguments"],
    ["loop-fusion", "4.8.9/2.4", "adjacent loops with the same header touching the same arrays: two memory passes where one would do; fuse"],
    ["loop-interchange", "1.3/4.8.8", "perfect nest whose inner loop walks the slow dimension (a[j][i] / a[j*n+i] with j inner) and nothing walks the fast one: swap the loops"],
    ["cache-tiling", "1.3/4.8.8", "nest where one access is unit-stride and another strided in the inner loop (transpose, matmul shapes): no order fixes both; block the loops"],
    ["kind-dispatch", "4.4.4/2.4", "per-element switch / if-chain on a type or kind field: mispredicts and cold code per element; bucket or sort by kind, one tight loop per kind"],
    ["flag-filter", "2.4/3.1", "loop skips or guards each element on a live/active flag or an empty slot: existence-based processing; keep a dense list of the live entries"],
    ["random-gather", "3.1.2/12.1", "a[idx[i]] gather or scatter into a table too large to stay in cache: sort or bucket the indices, or prefetch a[idx[i + d]]"],
    ["xtu-call", "4.2/10.3", "per-element call to a function defined (non-static) in another translation unit: cannot inline without LTO; a batch API over the array amortises the call"],
    ["ptr-to-index", "2.2/3.1.2", "[compiler] node struct with 2+ self-links: 32-bit pool indices instead of 8-byte pointers shrink every node (bytes measured after the best field order)"],
    ["wide-enum-field", "2.1", "[compiler] enum-typed field (4 B) whose enumerators fit in a byte: uint8_t storage shrinks the struct (bytes measured after the best field order)"],
    ["line-straddle", "2.3", "[compiler] struct iterated as an array whose size splits cache lines: that share of elements touches one more line than needed; pad or shrink to a divisor of 64"],
    ["many-streams", "4.8.9", "6+ distinct arrays indexed by the induction variable: register pressure, fission candidate"],
    ["loop-carried", "4.8.8", "x[i] computed from x[i-1]: serial dependency (prefix scan): cannot vectorize as written"],
    ["syscall-in-loop", "4.1/6.6", "system call inside a loop: batch it (writev/sendmmsg), cache it (time, pid), or hoist it"],
    ["lock-in-loop", "7.5", "lock or atomic read-modify-write inside a loop: accumulate locally, publish once"],
    ["format-in-loop", "9.1", "printf-family formatting inside a loop: per-element string formatting"],
    ["pointer-chase", "3.1.2/12.1", "p = p->next traversal: serialised cache misses; flat array, or prefetch ahead"],
    ["macro-list-walk", "3.1.2/12.1", "list_for_each-style macro loop: pointer chase the lexical loop finder cannot see"],
    ["bytewise-scan", "6.8.2", "byte-at-a-time delimiter scan: memchr / SIMD class scan"],
    ["regex-compile-in-loop", "19 (regex)", "regular expression compiled inside a loop: compile once"],
    ["aos-partial-touch", "2.4", "loop reads 1-2 fields of an array of structs with 4+ fields: SoA candidate"],
    ["binary-search", "12.8", "binary search loop: Eytzinger layout or branchless search if the table is large and static"],
    ["sort-call", "12.9", "qsort-family call: indirect comparator per compare; radix / inline sort for integer keys"],
    ["big-memset", "5.1 + hardening", "memset/calloc of 1 KiB+ in a function body: clearing may cost more than the work"],
    ["linear-realloc", "5.1/3.2", "realloc growing by a constant: O(n^2) copying; grow geometrically"],
    ["write-sequence", "6.3", "two or more write/send calls on the same fd in one function: writev/sendmsg candidate"],
    ["mmap-no-madvise", "5.8/6.5", "file maps memory but never advises the kernel of the access pattern"],
    ["seq-cst-atomic", "7.4", "atomic operation with the default (seq_cst) ordering: use the weakest correct ordering"],
    ["bool-fields", "2.1", "struct with 3+ bool/BOOL fields (BOOL is 4 bytes here): pack into a flags word"],
    ["false-sharing-review", "2.3/3.3.1", "struct with 2+ atomics (or atomics beside a lock) and no 64-byte alignment"],
    ["chained-hash", "3.1.1/3.1.2", "hash table with linked-list buckets (self-linked node + bucket pointer array)"],
    ["goto-loop", "4.4/4.8", "loop built from a backward goto (invisible to the for/while finder and to the vectorizer)"],
    ["local-heap-temp", "4.3/5.2", "constant-size heap allocation freed in the same function: stack, scratch or arena candidate"],
    ["alloc-heavy-fn", "5.1-5.3", "function with 4+ allocation calls (inventory; rank by heat)"],
    ["cold-path-in-hot-fn", "4.8.3", "hot function (profile) carrying error/throw/log calls not marked unlikely: cold code in the hot stream"],
    ["inline-too-costly", "4.2", "[compiler] call in a hot function the inliner refused (cost over threshold or noinline)"],
    ["slp-missed-hot", "4.8.8", "[compiler] hot function where the SLP vectorizer tried and gave up"],
    ["interleave-only", "4.4.5", "[compiler] loop 'vectorized' with width 1: only interleaved, no SIMD"],
    ["reorder-savings", "2.3", "[compiler] struct whose fields sorted by alignment would be smaller (bytes saved, cache lines)"],
    ["hot-fields-spread", "1.3/2.3", "[compiler+profile] large struct whose hot-path fields span more cache lines than they need"],
    ["fsync-call", "6.4", "fsync/fdatasync/F_FULLFSYNC call (durability vs latency: fdatasync where metadata is not needed)"],
    ["tcp-nodelay-missing", "6.6/8.5", "file creates TCP sockets but never sets TCP_NODELAY"],
    ["ptr-repeated-subscript", "4.8.1/ptr", "the same a[expr] is evaluated 3+ times in one loop body: take T* e = &a[expr] once"],
    ["ptr-repeated-chain", "4.8.7/ptr", "a 2+ hop member chain (a->b->c) repeats 3+ times in a loop: cache the intermediate pointer"],
    ["ptr-member-base-index", "4.8.6/ptr", "x->buf[i] indexed in a loop that also stores through pointers: the base is reloaded every iteration; hoist T* buf = x->buf"],
    ["ptr-row-pointer", "4.8.5/ptr", "a[i * n + j] in a loop over j: hoist the row pointer T* row = a + i * n"],
    ["ptr-struct-by-value", "4.8.4/ptr", "[compiler] struct of 32+ B passed or returned by value: pass const T*, return through T* out"],
    ["ptr-struct-copy-in-loop", "4.8.4/ptr", "struct element copied by value in a loop (T v = arr[i]) and only read: const T* v = &arr[i]"],
    ["ptr-prev-unlink", "3.x/ptr", "list unlink with a prev variable and a special-cased head: a T** pp cursor removes both branches"],
    ["ptr-array-of-pointers", "3.1.2/12.1", "items[i]->field in a loop: one dependent load per element through an array of pointers; store the records contiguously"],
    ["ptr-restrict-candidate", "4.8.6/ptr", "[compiler] loop refused for possible aliasing while the function takes 2+ same-type pointers: restrict-qualify them"],
    ["ptr-strlen-repeat", "4.8.1/ptr", "strlen() of the same unmodified string 2+ times in one function: keep the length (or an end pointer)"],
    ["stack-array", "hardening", "local array of 256 B+ that -ftrivial-auto-var-init=zero clears on every call"],
    ["single-vector-accumulator", "4.8.5/12.1", "SIMD loop whose reduction is ONE intrinsic chain (acc = vaddq(acc, ..)): latency-bound at one op per add latency; float chains want several accumulators"],
    ["branchy-merge", "4.4.4/12.9", "two-cursor merge whose step is an if on x[p] < y[q]: one mispredict per element on random data; branchless cursor advance"],
    ["fp-serial-reduction", "4.8.5/12.1", "one float/double accumulator in an innermost loop: a serial add chain (strict FP; an arm64 'vectorized' verdict is an ORDERED reduction, still serial): use several accumulators"],
    ["licm-blocked", "4.8.1/4.8.6", "[compiler] loads the loop may invalidate: not hoisted (aliasing store or call)"],
    ["gvn-clobbered", "4.8.1/4.8.6", "[compiler] loads not eliminated because a call/store clobbers them"],
    ["loop-spills", "4.8.9", "[compiler] register spills/reloads generated inside the loop"],
    ["padding", "2.3", "[compiler] struct padding holes reported by -Wpadded"],
];
const CHECK_IDS = CHECKS.map(c => c[0]);

const GENERIC = {
    alloc: String.raw`\w*(?:malloc|calloc|realloc|strn?dup|aligned_alloc|posix_memalign|_alloc|alloc_)\w*`,
    free: String.raw`free|\w*_free\w*|free_\w+|\w*(?:dealloc|release|destroy|dispose|unref)\w*`,
    push: String.raw`\w*_(?:push|append|put)\w*`,
    cold: String.raw`abort|perror|fprintf|err|errx|warn|warnx|\w*(?:[Tt]hrow|_die|[Ff]atal|[Pp]anic|_abort|assert_fail)\w*`,
    regex_compile: String.raw`regcomp|pcre2?_compile\w*|\w*re_compile\w*|\w*compile_regexp\w*|\w*[Rr]eg[Ee]xp?_?[Cc]ompile\w*`,
    sort: String.raw`qsort|qsort_r|mergesort|heapsort|\w*_qsort|rqsort`,
};

const alt = (generic, extra) => [generic, ...extra.map(x => (x.startsWith("re:") ? x.slice(3) : reEscape(x)))].filter(Boolean).map(p => "(?:" + p + ")").join("|");

let ALLOC_RE, FREE_RE, PUSH_RE, COLD_RE, JSAPI_RE, REGEX_RE, SORT_RE, TYPE_SIZE;
function configure(root, configFile) {
    if (root) ROOT = abspath(root);
    PROJECT = {};
    for (const cand of (configFile ? [configFile] : [join(ROOT, "dod_scan.json"), join(ROOT, "tools", "dod_scan.json")])) {
        if (cand && exists(cand)) { PROJECT = JSON.parse(readText(cand)); PROJECT._file = cand; break; }
    }
    DEFAULT_OUT = join(ROOT, PROJECT.out || ".dod-scan");
    ALLOC_RE = new RegExp(String.raw`\b(` + alt(GENERIC.alloc, PROJECT.alloc || []) + String.raw`)\s*\(`);
    FREE_RE = new RegExp(String.raw`\b(` + alt(GENERIC.free, PROJECT.free || []) + String.raw`)\s*\(`);
    PUSH_RE = new RegExp(String.raw`\b(` + alt(GENERIC.push, PROJECT.push || []) + String.raw`)\s*\(`);
    COLD_RE = new RegExp(String.raw`(?<![\w.>])(` + alt(GENERIC.cold, PROJECT.cold || []) + String.raw`)\s*\(`);
    REGEX_RE = new RegExp(String.raw`\b(` + alt(GENERIC.regex_compile, PROJECT.regex_compile || []) + String.raw`)\s*\(`);
    SORT_RE = new RegExp(String.raw`\b(` + alt(GENERIC.sort, PROJECT.sort || []) + String.raw`)\s*\(`);
    const api = PROJECT.api_prefixes || [];
    JSAPI_RE = api.length ? new RegExp(String.raw`\b(?:` + api.map(reEscape).join("|") + String.raw`)\w*\s*\(`) : /(?!x)x/;
    TYPE_SIZE = Object.assign({}, BASE_TYPE_SIZE, PROJECT.type_sizes || {});
    CS_MEMO = new Map();
    CS_ANY = new RegExp([ALLOC_RE, FREE_RE, PUSH_RE, COLD_RE, REGEX_RE, SORT_RE, JSAPI_RE, LIBM_RE, SYSCALL_RE, LOCK_RE, FMT_RE, LISTWALK_RE].map(r => "(?:" + r.source + ")").join("|"), "y");
}

const F_ALLOC = 1, F_FREE = 2, F_PUSH = 4, F_COLD = 8, F_REGEX = 16, F_SORT = 32, F_JSAPI = 64, F_LIBM = 128, F_SYSCALL = 256,
    F_LOCK = 512, F_FMT = 1024, F_LISTWALK = 2048, F_MEMX = 4096, F_MEMSET = 8192, F_CALLOCW = 16384, F_REALLOCW = 32768,
    F_ALLOCASSIGN = 65536, F_WRITE = 131072, F_STRLEN = 262144;
let CS_MEMO = new Map(), CS_ANY = null;
function csMask(w) {
    let m = CS_MEMO.get(w);
    if (m !== undefined) return m;
    const c = w + "(";
    m = 0;
    CS_ANY.lastIndex = 0;
    if (CS_ANY.test(c)) {
        if (matchAt(ALLOC_RE, c)) m |= F_ALLOC;
        if (matchAt(FREE_RE, c)) m |= F_FREE;
        if (matchAt(PUSH_RE, c)) m |= F_PUSH;
        if (matchAt(COLD_RE, c)) m |= F_COLD;
        if (matchAt(REGEX_RE, c)) m |= F_REGEX;
        if (matchAt(SORT_RE, c)) m |= F_SORT;
        if (matchAt(JSAPI_RE, c)) m |= F_JSAPI;
        if (matchAt(LIBM_RE, c)) m |= F_LIBM;
        if (matchAt(SYSCALL_RE, c)) m |= F_SYSCALL;
        if (matchAt(LOCK_RE, c)) m |= F_LOCK;
        if (matchAt(FMT_RE, c)) m |= F_FMT;
        if (matchAt(LISTWALK_RE, c)) m |= F_LISTWALK;
    }
    if (w === "memcpy" || w === "memmove" || w === "memset") m |= F_MEMX;
    if (w === "memset") m |= F_MEMSET;
    if (w.includes("calloc") || w.includes("mallocz")) m |= F_CALLOCW;
    if (w.includes("realloc")) m |= F_REALLOCW;
    if (w.includes("malloc") || w.includes("calloc") || w.includes("_alloc")) m |= F_ALLOCASSIGN;
    if (w === "write" || w === "send" || w === "sendto") m |= F_WRITE;
    if (w === "strlen") m |= F_STRLEN;
    CS_MEMO.set(w, m);
    return m;
}
const CS_RE = /\b(\w+)\s*\(/g;
function callSites(s) {
    const pos = [], end = [], word = [], mask = [], prev = [];
    CS_RE.lastIndex = 0;
    let m;
    while ((m = CS_RE.exec(s)) !== null) {
        const w = m[1];
        pos.push(m.index); end.push(m.index + m[0].length); word.push(w); mask.push(csMask(w)); prev.push(m.index ? s.charCodeAt(m.index - 1) : -1);
    }
    return { pos, end, word, mask, prev };
}
function csLower(cs, lo) {
    const p = cs.pos;
    let a = 0, b = p.length;
    while (a < b) { const h = (a + b) >> 1; if (p[h] < lo) a = h + 1; else b = h; }
    return a;
}
function csHits(cs, lo, hi, bit, lb = false) {
    const out = [];
    const { pos, mask, prev } = cs;
    for (let k = csLower(cs, lo); k < pos.length && pos[k] < hi; k++) {
        if (!(mask[k] & bit)) continue;
        if (lb && pos[k] !== lo && (prev[k] === 46 || prev[k] === 62)) continue;
        out.push(k);
    }
    return out;
}
function csAny(cs, lo, hi, bit) {
    const { pos, mask } = cs;
    for (let k = csLower(cs, lo); k < pos.length && pos[k] < hi; k++) if (mask[k] & bit) return true;
    return false;
}
const csWords = (cs, lo, hi, bit, lb = false) => csHits(cs, lo, hi, bit, lb).map(k => cs.word[k]);

const SMALLCPY_RE = new RegExp(String.raw`\b(memcpy|memmove|memset)\s*\(([^;]*?),\s*(1|2|3|4|8|16|sizeof\s*\(\s*(u?int(8|16|32|64)_t|float|double|` +
    String.raw`int|unsigned|short|char|size_t|uint32_t|uint64_t)\s*\))\s*\)`);
const LIBM_RE = new RegExp(String.raw`\b(sqrtf?|cbrtf?|sinf?|cosf?|tanf?|expf?|exp2f?|logf?|log2f?|log10f?|powf?|fmodf?|floorf?|ceilf?|roundf?|` +
    String.raw`lroundf?|truncf?|hypotf?|atan2f?|fabsf?|fminf?|fmaxf?|isnan|isfinite|isinf)\s*\(`);
const FNPTR_RE = /(->\s*\w+\s*\(|\(\s*\*\s*\w+\s*\)\s*\(|\b\w+\s*\[\s*[^\]]+\]\s*\()/;
const DIV_RE = /[^/*]\/\s*\(?\s*[A-Za-z_][\w.>-]*/;
const LOOP_RE = /\b(for|while|do)\b/;
const LOCALARR_RE = new RegExp(String.raw`(?:(?<=[{;}\n])|^)[ \t]*(?:const\s+|static\s+)?([A-Za-z_][\w]*(?:\s+[A-Za-z_]\w*)?(?:\s*\*+)?)\s+([A-Za-z_]\w*)\s*` +
    String.raw`((?:\[\s*[^\]\[]+\s*\])+)\s*(?:DYN_IO_SCRATCH\s*)?[;=,]`);
const BASE_TYPE_SIZE = { char: 1, int8_t: 1, uint8_t: 1, "unsigned char": 1, "signed char": 1, bool: 1, BOOL: 4,
    short: 2, int16_t: 2, uint16_t: 2, int: 4, unsigned: 4, int32_t: 4, uint32_t: 4,
    float: 4, long: 8, int64_t: 8, uint64_t: 8, double: 8, size_t: 8, uintptr_t: 8 };
const KEYWORDS = new Set(("if else for while do return break continue switch case default goto sizeof struct union enum " +
    "typedef static const volatile unsigned signed int char short long float double void inline " +
    "likely unlikely NULL true false TRUE FALSE _Alignof defined").split(" "));
const MOD_RE = /(?<![%])%\s*=?\s*(\(?\s*[A-Za-z_][\w.>\-]*|\d+)/;
const SYSCALL_RE = new RegExp(String.raw`(?<![\w.>])(read|write|pread|pwrite|readv|writev|send|recv|sendto|recvfrom|sendmsg|recvmsg|sendmmsg|` +
    String.raw`clock_gettime|gettimeofday|time|getpid|gettid|stat|fstat|lstat|lseek|fsync|fdatasync|fcntl|ioctl|` +
    String.raw`epoll_ctl|kevent|poll|select|open|openat|close|access|mmap|munmap|mprotect|getrusage|sched_yield|` +
    String.raw`nanosleep|usleep|getenv|sysconf|mach_absolute_time)\s*\(`);
const SYSCALL_KIND = { clock_gettime: "time", gettimeofday: "time", time: "time", mach_absolute_time: "time",
    getpid: "meta", gettid: "meta", getenv: "meta", sysconf: "meta", getrusage: "meta",
    stat: "meta", fstat: "meta", lstat: "meta", access: "meta",
    fcntl: "fdctl", ioctl: "fdctl", epoll_ctl: "fdctl", kevent: "fdctl" };
const LOCK_RE = new RegExp(String.raw`\b(\w*mutex_lock|\w*rwlock_\w*lock|\w*spin_lock|\w*unfair_lock_lock|\w*Lock\w*Mutex|\w+_lock|atomic_fetch_\w+|atomic_exchange\w*|` +
    String.raw`atomic_compare_exchange\w*|__atomic_fetch_\w+|__atomic_\w*exchange\w*|__sync_\w+)\s*\(`);
const FMT_RE = /\b(\w*printf)\s*\(/;
const CHASE_RE = /(?<![\w.>])([A-Za-z_]\w*)\s*=\s*\1\s*->\s*([A-Za-z_]\w*)\s*[;),]/;
const LISTWALK_RE = /\b(list_for_each\w*|LIST_FOREACH\w*|TAILQ_FOREACH\w*|SLIST_FOREACH\w*|hlist_for_each\w*)\s*\(/;
const BYTECMP_RE = new RegExp(String.raw`(\*\s*\(?\s*[A-Za-z_][\w.>\-]*\s*\)?|[A-Za-z_][\w.>\-]*\s*\[[^\]]+\])\s*(==|!=)\s*'[^'\n]{1,4}'` +
    String.raw`|'[^'\n]{1,4}'\s*(==|!=)\s*(\*\s*[A-Za-z_]\w*|[A-Za-z_][\w.>\-]*\s*\[[^\]]+\])`);
const BSEARCH_RE = new RegExp(String.raw`\(\s*([A-Za-z_]\w*)\s*\+\s*([A-Za-z_]\w*)\s*\)\s*(?:\/\s*2\b|>>\s*1\b)|` +
    String.raw`\b([A-Za-z_]\w*)\s*\+\s*\(\s*\(?\s*([A-Za-z_]\w*)\s*-\s*\3\s*\)?\s*(?:\/\s*2\b|>>\s*1\b)`);
const MEMSET_RE = /\bmemset\s*\(\s*([^,]+?)\s*,\s*0\s*,\s*([^;]+?)\)\s*;/;
const CALLOC_RE = /\b(\w*calloc\w*|\w*mallocz\w*)\s*\(([^;]*)\)/;
const REALLOC_RE = /\b(\w*realloc\w*)\s*\(([^;]*)\)\s*;/;
const WRITE_RE = /(?<![\w.>])(write|send|sendto)\s*\(\s*([A-Za-z_][\w.>\-]*)\s*,/;
const ATOMIC_DEFAULT_RE = new RegExp(String.raw`\b(atomic_(?:load|store|fetch_add|fetch_sub|fetch_or|fetch_and|fetch_xor|exchange|` +
    String.raw`compare_exchange_strong|compare_exchange_weak))\s*\(|__ATOMIC_SEQ_CST|memory_order_seq_cst`);
const VACC_RE = new RegExp(String.raw`(?<![\w.>])([A-Za-z_]\w*)\s*=\s*((?:v(?:add|fma|mla|pada|padal|dot|max|min|qadd|abal|aba)\w*|` +
    String.raw`_mm\d*_(?:add|fmadd|fmsub|fnmadd|max|min|dpbusd|madd|sad)\w*|` +
    String.raw`sv(?:add|mla|mad|max|min|dot)\w*))\s*\(\s*(?:[A-Za-z_]\w*\s*,\s*)?([^,()]*\([^()]*\)[^,()]*|[^,()]+)?`, "d");
const STRUCT_RE = /(?:typedef\s+)?struct\s+([A-Za-z_]\w*)?\s*\{/;

const BLANK_NEXT = /[\/"']|(?<=\n)#/g;
const BLANK_STR = { '"': /(?:[^"\\]|\\[\s\S]?)*/y, "'": /(?:[^'\\]|\\[\s\S]?)*/y };
const isJsSpace = (c) => (c >= 9 && c <= 13) || c === 32 || c === 160 || c === 5760 || (c >= 8192 && c <= 8202) || c === 8232 || c === 8233 || c === 8239 || c === 8287 || c === 12288 || c === 65279;
const isWordCode = (c) => (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95;
const LOCALARR_Y = new RegExp(LOCALARR_RE.source, "y");
function localArrays(body) {
    const out = [];
    let last = 0, r = body.indexOf("[");
    while (r >= 0) {
        let q = r;
        for (;;) {
            if (q <= last) break;
            const c = body.charCodeAt(q - 1);
            if (!(isWordCode(c) || c === 42 || isJsSpace(c))) break;
            q--;
        }
        let found = null;
        for (let p = q; p < r && found === null; p++) {
            const pc = p === 0 ? -1 : body.charCodeAt(p - 1);
            if (p !== 0 && pc !== 123 && pc !== 59 && pc !== 125 && pc !== 10) continue;
            LOCALARR_Y.lastIndex = p;
            found = LOCALARR_Y.exec(body);
        }
        if (found !== null) { out.push(found); last = mEnd(found); r = body.indexOf("[", last); }
        else r = body.indexOf("[", r + 1);
    }
    return out;
}

function blankCommentsAndStrings(text) {
    const out = [];
    let i = 0;
    const n = text.length;
    const blank = (s, fill) => (s.includes("\n") ? s.split("\n").map(l => fill.repeat(l.length)).join("\n") : fill.repeat(s.length));
    while (i < n) {
        const c = text[i];
        if (c === "/" && i + 1 < n && text[i + 1] === "*") {
            let j = text.indexOf("*/", i + 2);
            j = j < 0 ? n : j + 2;
            out.push(blank(text.slice(i, j), " "));
            i = j;
        } else if (c === "/" && i + 1 < n && text[i + 1] === "/") {
            let j = text.indexOf("\n", i);
            j = j < 0 ? n : j;
            out.push(" ".repeat(j - i));
            i = j;
        } else if (c === '"' || c === "'") {
            const sr = BLANK_STR[c];
            sr.lastIndex = i + 1;
            sr.exec(text);
            let j = sr.lastIndex;
            j = Math.min(j + 1, n);
            const fill = c === "'" ? "x" : " ";
            out.push(c + blank(text.slice(i + 1, j - 1 < i + 1 ? i + 1 : j - 1), fill) + (j - 1 > i ? c : ""));
            i = j;
        } else if (c === "#" && (i === 0 || text[i - 1] === "\n")) {
            let j = i, k;
            for (;;) {
                k = text.indexOf("\n", j);
                if (k < 0) { k = n; break; }
                if (text[k - 1] === "\\") { j = k + 1; continue; }
                break;
            }
            out.push(blank(text.slice(i, k), " "));
            i = k;
        } else {
            BLANK_NEXT.lastIndex = i + 1;
            const nx = BLANK_NEXT.exec(text);
            const j = nx === null ? n : nx.index;
            out.push(text.slice(i, j));
            i = j;
        }
    }
    return out.join("");
}

function matchParen(s, i, o, c) {
    if (i >= 0) {
        let d = 0, po = s.indexOf(o, i), pc = s.indexOf(c, i);
        while (pc >= 0) {
            if (po >= 0 && po < pc) { d++; po = s.indexOf(o, po + 1); }
            else { d--; if (d === 0) return pc; pc = s.indexOf(c, pc + 1); }
        }
        return -1;
    }
    let depth = 0;
    for (let j = i; j < s.length; j++) {
        const ch = s.at(j);
        if (ch === o) depth++;
        else if (ch === c) { depth--; if (depth === 0) return j; }
    }
    return -1;
}

function statementEnd(s, i) {
    let j = i < 0 ? s.length + i : i;
    while (j < s.length && " \t\n".includes(s[j])) j++;
    if (j < s.length && s[j] === "{") return matchParen(s, j, "{", "}");
    const m = matchAt(LOOP_RE, s, j);
    if (m && (m[1] === "for" || m[1] === "while")) {
        const p = s.indexOf("(", mEnd(m));
        const q = p >= 0 ? matchParen(s, p, "(", ")") : -1;
        return q >= 0 ? statementEnd(s, q + 1) : -1;
    }
    if (s.startsWith("if", j) && matchAt(/if\b/, s, j)) {
        const p = s.indexOf("(", j);
        const q = matchParen(s, p, "(", ")");
        const e = statementEnd(s, q + 1);
        let k = e + 1;
        while (k < s.length && " \t\n".includes(s[k])) k++;
        if (s.startsWith("else", k) && matchAt(/else\b/, s, k)) return statementEnd(s, k + 4);
        return e;
    }
    let depth = 0;
    for (let k = j; k < s.length; k++) {
        if ("([{".includes(s[k])) depth++;
        else if (")]}".includes(s[k])) depth--;
        else if (s[k] === ";" && depth <= 0) return k;
    }
    return -1;
}

function defines(paths) {
    const key = BLAKE3Hex([cacheVersion(), ...paths.map(p => p + ":" + fileHash(p))].join("\n"));
    return new Map(resultCache("defines", key, () => [...definesRaw(paths)]));
}
function definesRaw(paths) {
    const d = new Map();
    for (const p of paths) {
        const t = readText(p);
        if (t === null || !t.includes("define")) continue;
        const lines = t.split("\n");
        if (lines.length && lines[lines.length - 1] === "") lines.pop();
        for (const line of lines) {
            if (!line.includes("define")) continue;
            let m = matchAt(/\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+\(?\s*(\d+)\s*(?:u|U|ull|ULL)?\s*\)?\s*$/, line);
            if (m) d.set(m[1], Number(BigInt(m[2])));
            m = matchAt(/\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+\(\s*1u?\s*<<\s*(\d+)\s*\)/, line);
            if (m) d.set(m[1], Number(1n << BigInt(m[2])));
        }
    }
    return d;
}

function evalDim(expr, defs) {
    expr = expr.trim();
    expr = expr.replace(/\b([A-Z_][A-Z0-9_]*)\b/g, (_, k) => (defs.has(k) ? String(defs.get(k)) : "X"));
    if (expr.includes("X") || !fullmatch(/[\d\s+\-*/()<]+/, expr)) return null;
    return pyEval(expr);
}

function assignedNames(body) {
    const out = new Set();
    for (const m of fiter(/(?<![\w.>])([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:[-+*/%&|^]|<<|>>)?=(?!=)/, body)) out.add(m[1]);
    for (const m of fiter(/(?<![\w.>])(?:\+\+|--)\s*([A-Za-z_]\w*)|(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--)/, body)) out.add(m[1] || m[2]);
    for (const m of fiter(/&\s*([A-Za-z_]\w*)/, body)) out.add(m[1]);
    return out;
}

function loopVar(kw, header) {
    if (kw !== "for") return null;
    const parts = header.split(";");
    if (parts.length !== 3) return null;
    const m = search(/(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|[-+]=)/, parts[2]) || search(/(?:\+\+|--)\s*([A-Za-z_]\w*)/, parts[2]);
    return m ? m[1] : null;
}

function freeIdents(expr) {
    const out = [];
    for (const m of fiter(/(?<![\w.>])([A-Za-z_]\w*)/, expr)) {
        const name = m[1];
        if (KEYWORDS.has(name) || fullmatch(/[A-Z][A-Z0-9_]+/, name)) continue;
        out.push(name);
    }
    return out;
}

const isPow2 = (n) => n > 0 && Number((BigInt(n) & (BigInt(n) - 1n))) === 0;
const lvRe = (lv) => reEscape(lv);

function gistLoopChecks(L, body, header, defs, cs, off) {
    const hi = off + body.length;
    const f = {};
    const lv = loopVar(L.kw, header);
    L.lv = lv || "";
    let assignedSet = null;
    const assigned = { has: (x) => (assignedSet ??= assignedNames(body)).has(x) };
    let cos = null;
    const callsOrStores = () => (cos ??= (() => {
        const i = body.indexOf(")");
        const bodyAfter = i < 0 ? body : body.slice(i + 1);
        return !!search(/(?<![\w.>])[A-Za-z_]\w*\s*\(/, bodyAfter) || !!search(/(->\s*\w+|\]|\*\s*\w+)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)/, body);
    })());
    const hasBr = body.includes("["), hasArrow = body.includes("->");
    const cond = L.kw === "for" && header.split(";").length - 1 === 2 ? header.split(";")[1] : header;
    let m = search(/(<=?|!=|>=?)\s*([A-Za-z_]\w*(?:\s*->\s*\w+|\s*\.\s*\w+)+)/, cond);
    if (m && callsOrStores() && !assigned.has(m[2].split("-")[0].split(".")[0].trim())) f["bound-reload"] = [norm(m[2])];
    const cm = findall(/(?<![\w.>])([A-Za-z_]\w*)\s*\(/, cond).filter(x => !KEYWORDS.has(x) && x !== "likely" && x !== "unlikely" && !fullmatch(/[A-Z][A-Z0-9_]+/, x));
    if (cm.length) f["call-in-bound"] = uniqSorted(cm);
    if (L.inner) {
        const inv = [];
        for (const im of fiter(/(?<![\w])if\s*\(/, body)) {
            const q = matchParen(body, mEnd(im) - 1, "(", ")");
            if (q < 0) continue;
            let c = body.slice(mEnd(im), q);
            c = c.replace(/\b(?:likely|unlikely)\s*\(/g, "(");
            if (c.includes("[") || search(/(?<![\w.>])[A-Za-z_]\w*\s*\(/, c) || search(/(?<![\w)\]])\*\s*\w/, c)) continue;
            const ids = freeIdents(c);
            if (ids.length && !ids.includes(lv) && !ids.some(x => assigned.has(x))) inv.push(norm(c).slice(0, 50));
        }
        if (inv.length) f["invariant-branch"] = inv;
        const cold = L.lines > 12 ? [] : csHits(cs, off, hi, F_COLD, true).filter(k => { const x = cs.pos[k] - off; return !body.slice(Math.max(0, x - 160), x).includes("unlikely"); }).map(k => cs.word[k]);
        if (cold.length && L.lines <= 12) f["cold-in-loop"] = uniqSorted(cold);
    }
    const mods = [];
    if (body.includes("%")) for (const mm of fiter(MOD_RE, body)) {
        const rhs = pyStrip(mm[1], "( ");
        if (isDigits(rhs)) { if (!isPow2(Number(rhs))) mods.push(rhs); }
        else if (defs.has(rhs)) { if (!isPow2(defs.get(rhs))) mods.push(rhs); }
        else mods.push(rhs);
    }
    if (mods.length) f["modulo-in-loop"] = uniqSorted(mods);
    if (L.inner && L.div) f["div-in-loop"] = [String(L.div)];
    if (lv && hasBr) {
        const e = lvRe(lv);
        const st = [];
        for (const sm of fiter(RX(String.raw`\[\s*([^\]\[]*\b` + e + String.raw`\b[^\]\[]*)\]`), body)) {
            const mm = search(RX(String.raw`\b` + e + String.raw`\s*\*\s*([A-Za-z_]\w*|\d+)|([A-Za-z_]\w*|\d+)\s*\*\s*\b` + e + String.raw`\b`), sm[1]);
            if (mm) { const k = mm[1] || mm[2]; if (k !== "1") st.push(k); }
        }
        if (st.length) f["non-unit-stride"] = uniqSorted(st);
        const bases = new Set(findall(RX(String.raw`([A-Za-z_][\w]*(?:\s*(?:->|\.)\s*\w+)*)\s*\[[^\]\[]*\b` + e + String.raw`\b`), body));
        if (bases.size >= 6) f["many-streams"] = [String(bases.size)];
        const stores = new Set(findall(RX(String.raw`([A-Za-z_]\w*)\s*\[\s*` + e + String.raw`\s*\]\s*=(?!=)`), body));
        const carried = [...stores].filter(b => search(RX(reEscape(b) + String.raw`\s*\[\s*` + e + String.raw`\s*-\s*1\s*\]`), body));
        if (carried.length) f["loop-carried"] = carried.sort();
        if (L.inner && search(RX(String.raw`[A-Za-z_][\w.>\-]*\s*\[[^\]]*\b` + e + String.raw`\b[^\]]*\]\s*=(?!=)`), body)) {
            const red = findall(/(?<![\w.>\]\)])([a-z_]\w*)\s*(?:\+=|-=|\*=|\|=|\^=)/, body)
                .filter(x => x !== lv && !search(RX(String.raw`\b` + reEscape(x) + String.raw`\s*\[`), body));
            if (red.length) f["transform-and-reduce"] = uniqSorted(red);
        }
        if (L.inner) {
            const aos = new Map();
            const add = (k, v) => { if (!aos.has(k)) aos.set(k, new Set()); aos.get(k).add(v); };
            for (const am of fiter(RX(String.raw`(?<![\w.>])([A-Za-z_]\w*)\s*\[[^\]\[]*\b` + e + String.raw`\b[^\]\[]*\]\s*\.\s*([A-Za-z_]\w*)`), body)) add(am[1], am[2]);
            for (const am of fiter(RX(String.raw`(?<![\w.>])([A-Za-z_]\w*)\s*\[[^\]\[]*\b` + e + String.raw`\b[^\]\[]*\]\s*->\s*([A-Za-z_]\w*)`), body)) add(am[1], am[2]);
            if (aos.size) {
                L._aos = new Map([...aos].filter(([, v]) => v.size <= 2).map(([k, v]) => [k, [...v].sort()]));
                L._aos_all = new Map([...aos].map(([k, v]) => [k, [...v].sort()]));
            }
        }
    }
    const acc = [], deep = [];
    if (hasArrow) for (const am of fiter(/(?<![\w.>])([A-Za-z_]\w*)((?:\s*(?:->|\.)\s*[A-Za-z_]\w*)+)\s*(\+=|-=|\*=|\|=|&=|\^=|\+\+|--)/, body)) {
        const base = am[1], chain = am[2];
        if (!chain.includes("->") || assigned.has(base) || base === lv) continue;
        acc.push(base + words(chain).join(""));
    }
    if (hasArrow) for (const am of fiter(/(?<![\w.>])([A-Za-z_]\w*)((?:\s*(?:->|\.)\s*[A-Za-z_]\w*){2,})\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)/, body)) {
        const base = am[1], chain = am[2];
        const arrows = chain.split("->").length - 1, dots = chain.split(".").length - 1;
        if (arrows >= 1 && arrows + dots >= 2 && !assigned.has(base) && base !== lv) deep.push(base + words(chain).join(""));
    }
    if (acc.length) f["accum-through-pointer"] = uniqSorted(acc).slice(0, 4);
    if (deep.length) f["deep-chain"] = uniqSorted(deep).slice(0, 4);
    const sc = csWords(cs, off, hi, F_SYSCALL, true);
    const io6 = new Set(["read", "write", "recv", "send", "close", "open"]);
    if (sc.length && !(body.includes("EINTR") && L.lines <= 6 && sc.every(x => io6.has(x))))
        f["syscall-in-loop"] = uniqSorted(sc.map(x => `${x}(${SYSCALL_KIND[x] || "io"})`));
    const lk = csWords(cs, off, hi, F_LOCK);
    if (lk.length) f["lock-in-loop"] = uniqSorted(lk);
    const fm = csWords(cs, off, hi, F_FMT);
    if (fm.length) f["format-in-loop"] = uniqSorted(fm);
    const ch = !hasArrow && !header.includes("->") ? [] : flist(CHASE_RE, header + ";" + body).map(x => `${x[1]}->${x[2]}`);
    if (ch.length) f["pointer-chase"] = [...uniqSorted(ch), ...(body.includes("__builtin_prefetch") ? ["prefetched"] : [])];
    if (L.inner && L.lines <= 5 && (search(BYTECMP_RE, header) || search(BYTECMP_RE, body)) && search(/\+\+|\+=\s*1\b/, header + body)) {
        const n = flist(BYTECMP_RE, header + body).length;
        f["bytewise-scan"] = [`${n} byte compare(s)`];
    }
    if (L.inner) {
        const accs = new Map();
        for (const vm of fiter(VACC_RE, body)) {
            const v = vm[1], fn = vm[2];
            const call = body.slice(vm.indices[2][1]);
            const q = matchParen(call, call.indexOf("("), "(", ")");
            const args = q > 0 ? call.slice(call.indexOf("(") + 1, q) : "";
            if (search(RX(String.raw`(?<![\w.>])` + reEscape(v) + String.raw`\b`), args) &&
                !search(RX(String.raw`\b[A-Za-z_]\w*\s+` + reEscape(v) + String.raw`\s*=`), body)) accs.set(v, fn);
        }
        if (accs.size === 1) {
            const [v, fn] = [...accs][0];
            const kind = search(/_f(16|32|64)\b|_p[sd]\b|_sd\b|_ss\b|f32|f64/, fn) ? "float" : "int";
            f["single-vector-accumulator"] = [`${v}=${fn} (${kind})`];
        }
    }
    if (L.inner && search(/\b(\w+)\s*<\s*[\w>.\-]+\s*(?:&&|\|\|)\s*\(?\s*(\w+)\s*</, header)) {
        for (const mm of fiter(/(?<![\w])if\s*\(([^;{]*?)\b([A-Za-z_]\w*)\s*\[\s*(\w+)\s*\]\s*(<|>|<=|>=)\s*([A-Za-z_]\w*)\s*\[\s*(\w+)\s*\]/, body)) {
            if (mm[3] !== mm[6] && mm[2] !== mm[5]) { f["branchy-merge"] = [`${mm[2]}[${mm[3]}] ${mm[4]} ${mm[5]}[${mm[6]}]`]; break; }
        }
    }
    const subs = counter();
    if (hasBr) for (const x of fiter(/(?<![\w.>])[A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*\s*\[[^\[\]]*[A-Za-z_][^\[\]]*\]/, body)) cinc(subs, norm(x[0]));
    let repSubs = [...subs].filter(([k, n]) => n >= 3 && !search(/\[\s*\d+\s*\]$/, k)).map(([k]) => k);
    repSubs = repSubs.filter(k => !assigned.has(k.split("[")[0].split("->")[0].split(".")[0].trim()));
    if (repSubs.length) f["ptr-repeated-subscript"] = sortedBy(repSubs, k => -subs.count(k)).slice(0, 3).map(k => `${k} x${subs.count(k)}`);
    const chains = counter();
    if (hasArrow) for (const x of fiter(/(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*){2,})/, body)) {
        const parts = norm(x[1]).split(/\s*(->|\.)\s*/);
        for (let k = 5; k < parts.length; k += 2) cinc(chains, parts.slice(0, k).join(""));
    }
    const repChains = [...chains].filter(([k, n]) => n >= 3 && k.includes("->") && !assigned.has(k.split("->")[0].split(".")[0].trim())).map(([k]) => k);
    if (repChains.length) f["ptr-repeated-chain"] = sortedBy(repChains, k => -chains.count(k)).slice(0, 3).map(k => `${k} x${chains.count(k)}`);
    if (lv && hasBr) {
        const e = lvRe(lv);
        let mb = uniqSorted(flist(RX(String.raw`(?<![\w.>])([A-Za-z_]\w*\s*->\s*\w+(?:\s*(?:->|\.)\s*\w+)*)\s*\[[^\]]*\b` + e + String.raw`\b`), body).map(x => norm(x[1])));
        mb = mb.filter(x => !assigned.has(x.split("->")[0].trim()));
        if (mb.length && callsOrStores()) f["ptr-member-base-index"] = mb.slice(0, 3);
        const rows = uniqSorted(flist(RX(String.raw`(?<![\w.>])[A-Za-z_]\w*\s*\[\s*([A-Za-z_]\w*)\s*\*\s*[A-Za-z_][\w>.\-]*\s*\+\s*` + e + String.raw`\s*\]`), body)
            .filter(x => x[1] !== lv).map(x => x[0]));
        if (rows.length) f["ptr-row-pointer"] = rows.slice(0, 3).map(norm);
        const apc = uniqSorted(flist(RX(String.raw`(?<![\w.>])([A-Za-z_][\w.>\-]*\s*\[\s*` + e + String.raw`\s*\])\s*->\s*\w+`), body).map(x => x[1]));
        if (apc.length) f["ptr-array-of-pointers"] = apc.slice(0, 3).map(norm);
        const cp = flist(RX(String.raw`(?:^|(?<=[;{}\n]))\s*(?:const\s+)?(?:struct\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*=\s*[A-Za-z_][\w.>\-]*\s*\[\s*` + e + String.raw`\s*\]\s*;`), body)
            .map(x => x[1]).filter(t => !KEYWORDS.has(t) && !(t in TYPE_SIZE) && !fullmatch(/u?int\d+_t|size_t|char|double|float|long|short|unsigned|JSValue|JSAtom/, t));
        if (cp.length) L._copytypes = uniqSorted(cp);
    }
    if (hasArrow && body.includes("prev") && search(/\bprev\s*->\s*next\s*=/, body) && search(/\belse\b[^;]*\b\w+\s*=\s*\w+\s*->\s*next\s*;|\bif\s*\(\s*!?\s*prev\s*\)/, body)) f["ptr-prev-unlink"] = ["prev-tracked unlink"];
    const rg = csWords(cs, off, hi, F_REGEX);
    if (rg.length) f["regex-compile-in-loop"] = uniqSorted(rg);
    const bsOk = (x) => (x.includes("/") || x.includes(">>")) && search(BSEARCH_RE, x);
    if (bsOk(body) || bsOk(header)) f["binary-search"] = ["midpoint"];
    const rl = [];
    if (csAny(cs, off, hi, F_REALLOCW)) for (const rm of fiter(REALLOC_RE, body)) {
        const a = rm[2];
        if (search(/\+\s*(\d+|[A-Za-z_]\w*)\s*\)?\s*\*?/, a) && !search(/\*\s*2|<<|\* *3 *\/ *2|\bcap\s*\*|grow|next_pow|\* *1\.5/, a)) rl.push(norm(a).slice(0, 50));
    }
    if (rl.length) f["linear-realloc"] = rl;
    return f;
}

const pySlice = (s, a, b) => { const n = s.length; const ix = (v, d) => (v === undefined ? d : v < 0 ? Math.max(0, n + v) : Math.min(v, n)); return s.slice(ix(a, 0), ix(b, n)); };
const trimEndAt = (s, end) => { while (end > 0 && " \t\n\r\f\v".includes(s[end - 1])) end--; return end; };
const setOf = (it) => new Set(it);
const union = (...ss) => { const o = new Set(); for (const s of ss) for (const x of s) o.add(x); return o; };
const inter = (a, b) => [...a].filter(x => b.has(x));
const minus = (a, b) => new Set([...a].filter(x => !b.has(x)));
const subset = (a, b) => [...a].every(x => b.has(x));

function parseStructs(s, lineOf, rel) {
    const out = [];
    for (const m of fiter(STRUCT_RE, s)) {
        const ob = mEnd(m) - 1;
        const cb = matchParen(s, ob, "{", "}");
        if (cb < 0) continue;
        const tail = matchAt(/\s*([A-Za-z_]\w*)?\s*;/, s.slice(cb + 1, cb + 80));
        const name = g(m, 1) || (tail && tail[1] ? tail[1] : null);
        const alias = tail && tail[1] ? tail[1] : null;
        if (!name) continue;
        const inner = s.slice(ob + 1, cb);
        let depth = 0, cur = [], fields = [];
        for (const ch of inner) {
            if (ch === "{" || ch === "(") depth++;
            else if (ch === "}" || ch === ")") depth--;
            if (ch === ";" && depth === 0) { fields.push(norm(cur.join(""))); cur = []; }
            else cur.push(ch);
        }
        fields = fields.filter(x => x);
        const bools = fields.filter(x => matchAt(/(?:const\s+)?(?:bool|BOOL|_Bool)\s+\w+\s*$/, x));
        const atomics = fields.filter(x => search(/\b_Atomic\b|\batomic_\w+\b|\bstd::atomic\b/, x));
        const locks = fields.filter(x => search(/\bpthread_mutex_t\b|\bpthread_spinlock_t\b|\bos_unfair_lock\b|\bSDL_Mutex\b/, x));
        const aligned = !!search(/_Alignas|alignas|aligned\s*\(\s*\(?\s*(64|128)/, inner);
        const selfnext = fields.filter(x => search(RX(String.raw`\bstruct\s+` + reEscape(name) + String.raw`\s*\*\s*(\w*next\w*|\w*link\w*|chain\w*)\b`), x)
            || (alias && search(RX(String.raw`\b` + reEscape(alias) + String.raw`\s*\*\s*(\w*next\w*|\w*link\w*|chain\w*)\b`), x)));
        const dptr = fields.filter(x => search(/\*\s*\*\s*(\w*(hash|bucket|table|tab|slot|chain)\w*)\s*$/i, x));
        out.push({ file: rel, line: lineOf[m.index], name, alias: alias || "",
            nfields: fields.length, bools: bools.length, atomics: atomics.length, locks: locks.length,
            aligned, selfnext: selfnext.length > 0, dptr });
    }
    return out;
}

const GOTO_LABEL_RE = /(?:^|(?<=[;{}\n]))[ \t]*([A-Za-z_]\w*)[ \t]*:(?![:=])/;
const ALLOC_ASSIGN_RE = /(?<![\w.>])([A-Za-z_][\w.>\-]*)\s*=\s*(?:\([^()]*\)\s*)?(\w*(?:malloc|calloc|mallocz|_alloc)\w*)\s*\(/;
const FSYNC_RE = /(?<![\w.>])(fsync|fdatasync)\s*\(|F_FULLFSYNC/;

const FIS_ASSIGN = new Set(["=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="]);
const FIS_TYPE_WORDS = new Set("const volatile static register unsigned signed long short struct union enum restrict".split(" "));
const FIS_BASE_TYPES = new Set(("void char short int long float double _Bool bool size_t ssize_t ptrdiff_t intptr_t uintptr_t " +
    "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t").split(" "));
const FIS_CTRL = new Set("if else for while do switch case default return break continue goto sizeof".split(" "));
const FIS_EXIT_RE = /(?<![\w.>])(?:break|continue|return|goto|longjmp|siglongjmp|exit|_exit|abort)\b/;
const FIS_PURE_VEC = new Set(("sqrt sqrtf fabs fabsf floor floorf ceil ceilf trunc truncf round roundf rint rintf nearbyint " +
    "fmin fminf fmax fmaxf copysign copysignf abs labs llabs fma fmaf").split(" "));
const FIS_INTRIN_PURE = new RegExp(String.raw`_mm\d*_(?!store|stream|maskstore|mask_store|mask_compressstore|i\d+scatter|mask_i\d+scatter)\w+` +
    String.raw`|v(?!st\d)[a-z]+[0-9]*q?_(?:lane_|laneq_|n_)?[a-z]+\d+(?:_[a-z]+\d+)?` +
    String.raw`|sv(?!st\d|stnt|scatter|prf)\w+`);
const FIS_PURE_SCALAR = new Set(("exp expf exp2 exp2f expm1 expm1f log logf log2 log2f log10 log10f log1p log1pf pow powf sin " +
    "sinf cos cosf tan tanf asin asinf acos acosf atan atanf atan2 atan2f sinh sinhf cosh coshf " +
    "tanh tanhf cbrt cbrtf hypot hypotf erf erff erfc erfcf lgamma lgammaf tgamma tgammaf fmod " +
    "fmodf remainder lround lroundf llround isnan isinf isfinite signbit").split(" "));

const FIS_TOKG = new RegExp(String.raw`[A-Za-z_]\w*` +
    String.raw`|(?:0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)[uUlLfF]*` +
    String.raw`|->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|[-+*/%&|^~!<>=?:,.;()\[\]{}]` +
    String.raw`|'[^'\n]*'|"[^"\n]*"|\S`, "g");
const FIS_OP1 = new Set("-+*/%&|^~!<>=?:,.;()[]{}".split(""));
function fisTokens(text) {
    const all = text.match(FIS_TOKG);
    if (all === null) return [];
    const out = new Array(all.length);
    for (let i = 0; i < all.length; i++) {
        const v = all[i], c = v.charCodeAt(0);
        let k;
        if (c === 95 || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)) k = "id";
        else if (c >= 48 && c <= 57) k = "num";
        else if (c === 39 || c === 34) { if (v.length < 2) return null; k = "lit"; }
        else if (v.length > 1) k = c === 46 ? "num" : "op";
        else if (FIS_OP1.has(v)) k = "op";
        else return null;
        out[i] = [k, v];
    }
    return out;
}

function fisMatch(toks, i, o, c) {
    let d = 0;
    for (let j = i; j < toks.length; j++) {
        const t = toks.at(j);
        const v = t[1];
        if (v === o) d++;
        else if (v === c) { d--; if (d === 0) return j; }
    }
    return -1;
}

function fisStatementEnd(toks, i) {
    if (i >= toks.length) return -1;
    const v = toks.at(i)[1];
    if (v === "{") return fisMatch(toks, i, "{", "}");
    if (["if", "switch", "for", "while"].includes(v) && i + 1 < toks.length && toks.at(i + 1)[1] === "(") {
        const q = fisMatch(toks, i + 1, "(", ")");
        if (q < 0) return -1;
        const e = fisStatementEnd(toks, q + 1);
        if (v === "if" && e >= 0 && e + 1 < toks.length && toks[e + 1][1] === "else") return fisStatementEnd(toks, e + 2);
        return e;
    }
    if (v === "do") {
        const e = fisStatementEnd(toks, i + 1);
        if (e < 0 || e + 2 >= toks.length || toks[e + 1][1] !== "while") return -1;
        const q = fisMatch(toks, e + 2, "(", ")");
        return q >= 0 && q + 1 < toks.length && toks[q + 1][1] === ";" ? q + 1 : -1;
    }
    let d = 0;
    for (let j = i; j < toks.length; j++) {
        const t = toks.at(j)[1];
        if (t === "(" || t === "[" || t === "{") d++;
        else if (t === ")" || t === "]" || t === "}") { d--; if (d < 0) return -1; }
        else if (t === ";" && d === 0) return j;
    }
    return -1;
}

function fisSplit(toks) {
    const out = [];
    let i = 0;
    while (i < toks.length) {
        if (toks[i][1] === ";") { i++; continue; }
        const e = fisStatementEnd(toks, i);
        if (e < 0) return null;
        out.push(toks.slice(i, e + 1));
        i = e + 1;
    }
    return out;
}

function fisIsDecl(st, base = 0) {
    const n = st.length;
    let k = base, tw = 0;
    while (k < n && FIS_TYPE_WORDS.has(st[k][1])) { k++; tw++; }
    if (k >= n || st[k][0] !== "id" || FIS_CTRL.has(st[k][1])) return null;
    const tagged = tw && ["struct", "union", "enum"].includes(st[k - 1][1]);
    if (tw && !tagged && k + 1 < n && ["=", ";", ",", "["].includes(st[k + 1][1])) return k - base;
    k++;
    while (k < n && ["*", "const", "restrict", "volatile"].includes(st[k][1])) k++;
    if (k >= n || st[k][0] !== "id" || FIS_CTRL.has(st[k][1]) || FIS_TYPE_WORDS.has(st[k][1])) return null;
    if (k + 1 < n && !["=", ";", ",", "["].includes(st[k + 1][1])) return null;
    return k - base;
}

class FisStmt {
    constructor(idx, toks) {
        this.idx = idx;
        this.toks = toks;
        this.text = toks.map(t => t[1]).join(" ");
        this.defs = new Map();
        this.uses = new Map();
        this.locals = new Set();
        this.calls = new Set();
        this.call_roots = new Set();
        this.roots = new Set();
        this.read_roots = new Set();
        this.decl_pos = new Set();
        this.block = [];
        this.work = [];
        this.counter_only = true;
        this.cond = toks.length ? ["if", "switch"].includes(toks[0][1]) : false;
        this.kind = toks.length && ["if", "switch", "for", "while", "do"].includes(toks[0][1]) ? toks[0][1] : "expr";
    }
    add(table, path, ix) { if (!table.has(path)) table.set(path, new Set()); table.get(path).add(ix); }
}

function fisChain(toks, i, end) {
    if (toks[i][0] !== "id") return null;
    const path = [toks[i][1]];
    const idxs = [];
    let j = i + 1;
    while (j < end) {
        const v = toks[j][1];
        if ((v === "->" || v === ".") && j + 1 < end && toks[j + 1][0] === "id") { path.push(v + toks[j + 1][1]); j += 2; }
        else if (v === "[") {
            const q = fisMatch(toks, j, "[", "]");
            if (q < 0 || q >= end) return null;
            idxs.push([j + 1, q]);
            path.push("[]");
            j = q + 1;
        } else break;
    }
    return [path.join(""), idxs, j];
}

function fisAnalyze(s, ivs, invariantOk, pureExtra) {
    const toks = s.toks;
    const n = toks.length;
    const declAt = fisIsDecl(toks);
    if (declAt !== null) {
        let k = declAt;
        while (k < n) {
            if (toks[k][0] === "id") { s.locals.add(toks[k][1]); s.decl_pos.add(k); s.add(s.defs, toks[k][1], ""); }
            let d = 0;
            while (k < n) {
                const v = toks[k][1];
                if (v === "(" || v === "[" || v === "{") d++;
                else if (v === ")" || v === "]" || v === "}") d--;
                else if ((v === "," || v === ";") && d === 0) break;
                k++;
            }
            k++;
            while (k < n && ["*", "const", "restrict"].includes(toks[k][1])) k++;
        }
    }
    let i = 0, prev = null;
    while (i < n) {
        const [kind, v] = toks[i];
        if (kind !== "id" || FIS_CTRL.has(v) || FIS_TYPE_WORDS.has(v) || FIS_BASE_TYPES.has(v)) { prev = v; i++; continue; }
        if (prev === "->" || prev === ".") { prev = v; i++; continue; }
        const ch = fisChain(toks, i, n);
        if (ch === null) return false;
        const [path, idxs, j] = ch;
        if (j < n && toks[j][1] === "(") {
            const q = fisMatch(toks, j, "(", ")");
            if (q < 0) return false;
            const name = path;
            if (name.includes("->") || name.includes(".") || name.includes("[]")) {
                s.calls.add("(*" + name + ")");
                s.block.push("indirect:" + name);
                s.counter_only = false;
            } else if ((FIS_PURE_VEC.has(name) || name.startsWith("__builtin_") || fullmatch(/[A-Z][A-Z0-9_]*/, name)
                || fullmatch(FIS_INTRIN_PURE, name) || pureExtra.has(name)) && i > 0) {
                for (let a = j + 1; a < q; a++) {
                    if (toks[a][0] === "id" && !ivs.has(toks[a][1]) && (a === j + 1 || [",", "(", "&"].includes(toks[a - 1][1]))) s.read_roots.add(toks[a][1]);
                }
            } else if (FIS_PURE_SCALAR.has(name)) {
                s.block.push("math:" + name);
            } else {
                s.calls.add(name);
                s.block.push("call:" + name);
                s.counter_only = false;
                if (q === j + 1 || (q === j + 2 && toks[j + 1][1] === "void")) s.call_roots.add("*");
                for (let a = j + 1; a < q; a++) {
                    if (toks[a][1] === "&" && a + 1 < q && toks[a + 1][0] === "id") s.call_roots.add(toks[a + 1][1]);
                    else if (toks[a][0] === "id" && (a === j + 1 || [",", "("].includes(toks[a - 1][1])) && !ivs.has(toks[a][1])) s.call_roots.add(toks[a][1]);
                }
            }
            prev = ")";
            i = j + 1;
            continue;
        }
        if (FIS_BASE_TYPES.has(path)) { prev = v; i++; continue; }
        const root = matchAt(/\w+/, path)[0];
        if (!ivs.has(root)) s.roots.add(root);
        const unaryStar = !s.decl_pos.has(i) && i > 0 && toks[i - 1][1] === "*" && (i === 1 || (toks[i - 2][0] === "op" && ![")", "]"].includes(toks[i - 2][1])));
        const preInc = i > 0 && (toks[i - 1][1] === "++" || toks[i - 1][1] === "--");
        const nxt = j < n ? toks[j][1] : "";
        const postInc = nxt === "++" || nxt === "--";
        const after = j + (postInc ? 1 : 0);
        const assign = after < n && FIS_ASSIGN.has(toks[after][1]) ? toks[after][1] : null;
        const ixText = idxs.map(([a, b]) => toks.slice(a, b).map(t => t[1]).join(" ")).join("|");
        if (unaryStar) {
            const starPath = "*" + path;
            if (assign || preInc) { s.add(s.defs, starPath, ""); s.work.push("store:" + starPath); s.counter_only = false; }
            if (assign !== "=") s.add(s.uses, starPath, "");
            if (postInc) { s.add(s.defs, path, ""); s.add(s.uses, path, ""); }
        } else {
            const isDef = !!assign || preInc || postInc;
            if (isDef) {
                s.add(s.defs, path, ixText);
                if (idxs.length) { s.work.push("store:" + path); s.counter_only = false; }
                else if (path.includes("->") || path.includes(".")) { s.work.push("acc:" + path); s.counter_only = false; }
                else if (["+=", "-=", "*=", "|=", "&=", "^="].includes(assign) || preInc || postInc) {
                    const rhs = [];
                    if (assign) { let e = after + 1; while (e < n && toks[e][1] !== ";") { rhs.push(toks[e]); e++; } }
                    if (assign && !rhs.every(t => t[0] === "num")) { s.work.push("reduce:" + path); s.counter_only = false; }
                }
            }
            if (assign !== "=" || !isDef) s.add(s.uses, path, ixText);
            if (isDef && idxs.length && s.cond) s.block.push("cond-store:" + path);
        }
        const wr = assign || preInc || postInc;
        for (const [a, b] of idxs) {
            const sub = toks.slice(a, b);
            if (sub.some(t => ["[", "->", "("].includes(t[1]))) s.block.push((wr ? "scatter:" : "gather:") + path);
            else {
                for (const t of sub) {
                    if (t[0] === "id" && !ivs.has(t[1]) && !invariantOk(t[1]) && !FIS_CTRL.has(t[1])) { s.block.push((wr ? "scatter:" : "gather:") + path); break; }
                }
            }
        }
        if (assign === "=" && !idxs.length && after + 1 < n) {
            const r = toks[after + 1][0] === "id" ? fisChain(toks, after + 1, n) : null;
            if (r && r[0].startsWith(path + "->") && r[2] < n && toks[r[2]][1] === ";") s.block.push("chase:" + path);
        }
        prev = v;
        i++;
    }
    if (s.kind === "switch") s.block.push("switch");
    for (const [path, ixs] of s.defs) {
        const uses = s.uses.get(path) || new Set();
        if (path.includes("[]") && [...ixs].some(d => [...uses].some(u => d && u && u !== d))) s.block.push("carried:" + path);
    }
    return true;
}

async function fisComponents(n, adj) {
    const gr = new Graph({ directed: true });
    for (let v = 0; v < n; v++) gr.addNode();
    adj.forEach((ws, v) => { for (const w of ws) gr.addEdge(v, w); });
    const { count, labels } = await gr.stronglyConnectedComponents();
    const comps = Array.from({ length: count }, () => []);
    for (let v = 0; v < n; v++) comps[labels[v]].push(v);
    return comps.filter(c => c.length);
}

function fisLoopParts(kw, header, body) {
    if (kw !== "for" && kw !== "while") return null;
    const p = body.indexOf("(");
    let d = 0, q = -1;
    if (p >= 0) {
        for (let j = p; j < body.length; j++) {
            if (body[j] === "(") d++;
            else if (body[j] === ")") { d--; if (d === 0) { q = j; break; } }
        }
    }
    if (p < 0 || q < 0) return null;
    const rest = lstripWs(body.slice(q + 1));
    if (!rest.startsWith("{") || !rstripWs(body).endsWith("}")) return null;
    return pySlice(rest, 1, rstripWs(rest).lastIndexOf("}"));
}

const FIS_WHY = [null];

async function fission(kw, header, body, pureExtra = new Set()) {
    const inner = fisLoopParts(kw, header, body);
    if (inner === null) { FIS_WHY[0] = "if inner is none"; return null; }
    const toks = fisTokens(inner);
    if (!toks || !toks.length) { FIS_WHY[0] = "if not toks"; return null; }
    const stmtsT = fisSplit(toks);
    if (!stmtsT || stmtsT.length < 2 || stmtsT.length > 60) { FIS_WHY[0] = "if not stmts_t or len stmts_t  < 2 or len stmts_t"; return null; }
    if (stmtsT.some(t => ["for", "while", "do"].includes(t[0][1]))) { FIS_WHY[0] = "if any t 0 1  in  for   while   do  for t in stmts"; return null; }
    for (const t of stmtsT) {
        let text = t.map(x => x[1]).join(" ");
        if (t[0][1] === "switch") text = text.replace(/\bbreak\s*;/g, ";");
        if (search(FIS_EXIT_RE, text)) { FIS_WHY[0] = "if fis_exit_re search text"; return null; }
    }
    let ivs = new Set();
    let condIds;
    if (kw === "for") {
        const parts = header.split(";");
        if (parts.length !== 3) { FIS_WHY[0] = "if len parts  != 3"; return null; }
        for (const m of fiter(/(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|[-+]=)|(?:\+\+|--)\s*([A-Za-z_]\w*)/, parts[2])) ivs.add(m[1] || m[2]);
        if (!ivs.size) { FIS_WHY[0] = "if not ivs"; return null; }
        condIds = minus(setOf(findall(/(?<![\w.>])([A-Za-z_]\w*)(?!\s*\()/, parts[1])), FIS_CTRL);
    } else {
        condIds = minus(setOf(findall(/(?<![\w.>])([A-Za-z_]\w*)(?!\s*\()/, header)), FIS_CTRL);
    }
    const ctrl = new Set();
    if (kw === "while") {
        stmtsT.forEach((t, si) => {
            const text = t.map(x => x[1]).join(" ");
            const m = fullmatch(/(?:(\w+) (?:\+\+|--)|(?:\+\+|--) (\w+)|(\w+) [-+]= [\w ()*+-]+) ;/, text);
            if (m) {
                const v = m[1] || m[2] || m[3];
                if (condIds.has(v)) { ivs.add(v); ctrl.add(si); }
            }
        });
        if (!ivs.size || [...ctrl].some(si => si < stmtsT.length - ctrl.size)) { FIS_WHY[0] = "if not ivs or any si < len stmts_t    len ctrl  fo"; return null; }
    }
    let stepIds = new Set();
    if (kw === "for") stepIds = minus(minus(setOf(findall(/(?<![\w.>])([A-Za-z_]\w*)/, header.split(";")[2])), ivs), FIS_CTRL);
    for (const si of ctrl) for (const [k, v] of stmtsT[si]) if (k === "id" && !ivs.has(v)) stepIds.add(v);
    const stmts = [];
    stmtsT.forEach((t, si) => { if (!ctrl.has(si)) stmts.push(new FisStmt(stmts.length, t)); });
    if (stmts.length < 2) { FIS_WHY[0] = "if len stmts  < 2"; return null; }
    const bodyDefs = new Set();
    for (const s of stmts) for (const p of fisQuickDefs(s.toks)) bodyDefs.add(p);
    const invariantOk = (name) => !bodyDefs.has(name);
    if (inter(stepIds, bodyDefs).length) { FIS_WHY[0] = "if step_ids   body_defs"; return null; }
    for (const s of stmts) if (!fisAnalyze(s, ivs, invariantOk, pureExtra)) { FIS_WHY[0] = "if not fis_analyze s  ivs  invariant_ok  pure_extr"; return null; }
    const bodyLocals = union(...stmts.map(s => s.locals));
    const rootOf = (p) => matchAt(/\*?(\w+)/, p)[1];
    for (const s of stmts) {
        for (const path of [...s.defs.keys(), ...s.uses.keys()]) {
            const root = rootOf(path);
            if (bodyLocals.has(root) && (path.includes("[]") || path.startsWith("*") || path.includes("->"))) s.block.push((s.defs.has(path) ? "scatter:" : "gather:") + path);
        }
    }
    for (const s of stmts) {
        for (const p of s.defs.keys()) {
            const rp = rootOf(p);
            if ((ivs.has(rp) || condIds.has(rp)) && !p.startsWith("*") && !p.includes("[]") && !p.includes("->") && !p.includes(".")) { FIS_WHY[0] = "if  rp in ivs or rp in cond_ids  and not p startsw"; return null; }
        }
    }
    const n = stmts.length;
    const adj = Array.from({ length: n }, () => new Set());
    const locals = new Map();
    for (const s of stmts) for (const v of s.locals) locals.set(v, s.idx);
    const paths = new Set();
    for (const s of stmts) { for (const p of s.defs.keys()) paths.add(p); for (const p of s.uses.keys()) paths.add(p); }
    const expand = new Set();
    for (const p of paths) {
        if (ivs.has(p)) continue;
        const acc = [];
        for (const s of stmts) for (const ix of (s.defs.get(p) || [])) acc.push([s.idx, ix, true]);
        for (const s of stmts) for (const ix of (s.uses.get(p) || [])) acc.push([s.idx, ix, false]);
        if (!acc.some(x => x[2])) continue;
        for (const [a, ia, da] of acc) {
            for (const [b, ib, db] of acc) {
                if (a >= b || !(da || db)) continue;
                if (locals.has(p)) {
                    if (locals.get(p) === a && da && !db) { adj[a].add(b); expand.add(p); continue; }
                    adj[a].add(b); adj[b].add(a);
                    continue;
                }
                const same = p.includes("[]") && ia === ib && ia !== "" && ["[", "->", "("].every(x => !ia.includes(x));
                adj[a].add(b);
                if (!same) adj[b].add(a);
            }
        }
    }
    for (const a of stmts) {
        for (const b of stmts) {
            const adr = setOf([...a.defs.keys()].map(rootOf));
            const bdr = setOf([...b.defs.keys()].map(rootOf));
            if (a.idx < b.idx && (inter(a.call_roots, b.roots).length || inter(b.call_roots, a.roots).length || (a.calls.size && b.calls.size)
                || inter(a.read_roots, bdr).length || inter(b.read_roots, adr).length
                || (a.call_roots.has("*") && b.roots.size) || (b.call_roots.has("*") && a.roots.size))) {
                adj[a.idx].add(b.idx);
                adj[b.idx].add(a.idx);
            }
        }
    }
    const comps = await fisComponents(n, adj);
    if (comps.length < 2) { FIS_WHY[0] = "if len comps  < 2"; return null; }
    const compOf = new Map();
    comps.forEach((c, ci) => { for (const v of c) compOf.set(v, ci); });
    const sortedComps = sortedBy(comps, c => c[0]);
    const pieces = [];
    for (const c of sortedComps) {
        pieces.push({ stmts: c, work: c.flatMap(v => stmts[v].work), block: c.flatMap(v => stmts[v].block),
            counter: c.every(v => stmts[v].counter_only), text: c.map(v => stmts[v].text.slice(0, 60)) });
    }
    const links = new Set();
    for (let a = 0; a < n; a++) for (const b of adj[a]) if (compOf.get(a) !== compOf.get(b)) links.add(compOf.get(a) + "," + compOf.get(b));
    const pieceOf = new Map();
    sortedComps.forEach((c, k) => { for (const v of c) pieceOf.set(v, k); });
    const elemOf = (p) => matchAt(/[^\[]*\[\]/, p);
    pieces.forEach((pc, k) => {
        pc.defs = setOf(pc.stmts.flatMap(v => [...stmts[v].defs.keys()]));
        pc.arr_reads = setOf(pc.stmts.flatMap(v => [...stmts[v].uses.keys()].filter(p => p.includes("[]") || p.startsWith("*"))));
        pc.elems = setOf(pc.stmts.flatMap(v => [...stmts[v].defs.keys(), ...stmts[v].uses.keys()].filter(p => p.includes("[]") && elemOf(p)[0] !== p).map(p => elemOf(p)[0])));
        let linked = false;
        for (let a = 0; a < n && !linked; a++) for (const b of adj[a]) if (pieceOf.get(a) !== pieceOf.get(b) && (pieceOf.get(a) === k || pieceOf.get(b) === k)) { linked = true; break; }
        pc.linked = linked;
    });
    const blocked = pieces.filter(p => p.block.length);
    const bdefs = union(...blocked.map(p => p.defs));
    const belems = union(...blocked.map(p => p.elems));
    const clean = pieces.filter(p => !p.block.length && p.work.some(w => w.startsWith("store:") || w.startsWith("reduce:")) && !p.counter
        && !inter(p.defs, bdefs).length && !inter(p.elems, belems).length && (!p.linked || minus(p.arr_reads, bdefs).size));
    if (!clean.length || !blocked.length) { FIS_WHY[0] = "if not clean or not blocked"; return null; }
    const crossLocals = [...expand].filter(v => stmts.some(s => s.uses.has(v) && compOf.get(locals.get(v)) !== compOf.get(s.idx))).sort();
    return { pieces, clean, blocked, independent: links.size === 0, expand: crossLocals };
}

function fisQuickDefs(toks) {
    const out = new Set();
    const n = toks.length;
    toks.forEach(([k, v], i) => {
        if (k === "op" && (FIS_ASSIGN.has(v) || v === "++" || v === "--")) {
            let j = i - 1;
            while (j >= 0 && toks[j][1] === "]") {
                let d = 0;
                while (j >= 0) {
                    if (toks[j][1] === "]") d++;
                    else if (toks[j][1] === "[") { d--; if (d === 0) break; }
                    j--;
                }
                j--;
            }
            if (j >= 0 && toks[j][0] === "id") out.add(toks[j][1]);
            if ((v === "++" || v === "--") && i + 1 < n && toks[i + 1][0] === "id") out.add(toks[i + 1][1]);
        }
    });
    const d = fisIsDecl(toks);
    if (d !== null) out.add(toks[d][1]);
    return out;
}

function fissionSummary(r) {
    const c = r.clean[0], b = r.blocked[0];
    const cw = uniqSorted(c.work.map(w => split1(w, ":")[1])).slice(0, 3);
    const bb = uniqSorted(b.block).slice(0, 3);
    const tail = r.independent ? "independent" : (r.expand.length ? "expand " + r.expand.slice(0, 3).join(",") + " into a temp array" : "ordered passes");
    return `${r.pieces.length} pieces: clean {${cw.join(",")}} | blocked by {${bb.join(",")}} (${tail})`;
}

function fisDeclNames(toks) {
    const out = new Set();
    const n = toks.length;
    for (let pos = 0; pos < n; pos++) {
        if (pos && ![";", "{", "}"].includes(toks[pos - 1][1]) && !(toks[pos - 1][1] === "(" && pos > 1 && toks[pos - 2][1] === "for")) continue;
        let k = fisIsDecl(toks, pos);
        if (k === null) continue;
        k += pos;
        while (k < n) {
            if (toks[k][0] === "id") out.add(toks[k][1]);
            let d = 0;
            while (k < n) {
                const v = toks[k][1];
                if (v === "(" || v === "[" || v === "{") d++;
                else if (v === ")" || v === "]" || v === "}") { d--; if (d < 0) break; }
                else if ((v === "," || v === ";") && d === 0) break;
                k++;
            }
            if (k >= n || toks[k][1] !== ",") break;
            k++;
            while (k < n && ["*", "const", "restrict"].includes(toks[k][1])) k++;
        }
    }
    return out;
}

function fisPureInfo(text, a, b) {
    const sig = Math.max(rfind(text, "}", 0, a), rfind(text, ";", 0, a)) + 1;
    const head = text.slice(sig, a);
    if (!search(/\bstatic\b/, head)) return null;
    const toks = fisTokens(text.slice(a + 1, b));
    const ptoks = head.includes("(") ? fisTokens(pySlice(head, head.indexOf("(") + 1, head.lastIndexOf(")"))) : [];
    if (toks === null || ptoks === null) return null;
    const locs = new Set();
    for (const part of ptoks.map(t => t[1]).join(" ").split(",")) {
        const w = words(part);
        if (w.length && !w.includes("*") && !part.includes("[") && !FIS_CTRL.has(w[w.length - 1])) locs.add(w[w.length - 1]);
    }
    const callees = new Set();
    const n = toks.length;
    if (toks.some(t => t[1] === "static")) return null;
    for (const x of fisDeclNames(toks)) locs.add(x);
    for (let i = 0; i < n; i++) {
        const [k, v] = toks[i];
        if (k === "op" && (FIS_ASSIGN.has(v) || v === "++" || v === "--")) {
            let j = FIS_ASSIGN.has(v) || (i > 0 && toks[i - 1][0] === "id") ? i - 1 : i + 1;
            if ((v === "++" || v === "--") && !(i > 0 && toks[i - 1][0] === "id")) j = i + 1;
            if (j < 0 || j >= n || toks[j][0] !== "id" || !locs.has(toks[j][1])) return null;
            if (j > 0 && ["->", ".", "*"].includes(toks[j - 1][1]) && !(toks[j - 1][1] === "*" && j > 1 && toks[j - 2][0] === "id")) return null;
        }
        if (k === "id" && i + 1 < n && toks[i + 1][1] === "(" && !FIS_CTRL.has(v) && (i === 0 || !["->", "."].includes(toks[i - 1][1]))) {
            if (v === "memcpy" || v === "memmove") {
                if (!((toks[i + 2][1] === "&" && locs.has(toks[i + 3][1])) || locs.has(toks[i + 2][1]))) return null;
            } else if (!(FIS_PURE_VEC.has(v) || FIS_PURE_SCALAR.has(v) || v.startsWith("__builtin_") || fullmatch(/[A-Z][A-Z0-9_]*/, v))) callees.add(v);
        }
    }
    return callees;
}

function fisPureOracle(text, funcs) {
    const byName = new Map();
    for (const [a, b, name] of funcs) { if (!byName.has(name)) byName.set(name, []); byName.get(name).push([a, b]); }
    const info = new Map(), memo = new Map(), busy = new Set();
    const infoOf = (name) => {
        if (info.has(name)) return info.get(name);
        let r = null;
        const defs = byName.get(name) || [];
        for (let k = defs.length - 1; k >= 0 && r === null; k--) r = fisPureInfo(text, defs[k][0], defs[k][1]);
        info.set(name, r);
        return r;
    };
    const pure = (name) => {
        if (memo.has(name)) return memo.get(name);
        if (busy.has(name)) return false;
        const cs = infoOf(name);
        let ok = cs !== null;
        if (ok) {
            busy.add(name);
            for (const c of cs) if (c !== name && !pure(c)) { ok = false; break; }
            busy.delete(name);
        }
        memo.set(name, ok);
        return ok;
    };
    return pure;
}

const DM_TAG_RE = /(?:^|_)(?:type|kind|tag|op|opcode|cls|class|variant|tid|ty)s?$/i;
const DM_FLAG_RE = new RegExp(String.raw`(?:^|_)(?:active|alive|used|valid|dead|deleted|removed|enabled|disabled|free|visible|dirty|live|` +
    String.raw`in_use|inuse|occupied|empty|tomb\w*|skip|hidden|done|alloc(?:ated)?|present|set)$`, "i");
const DM_KNOWN_EXT = new Set(("memcpy memmove memset memcmp strlen strcmp strncmp strchr strrchr strstr malloc calloc realloc free " +
    "printf fprintf snprintf sprintf puts putchar fputs fwrite fread abort assert qsort bsearch").split(" "));
const DM_ACC_RE = /(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)((?:\s*\[(?:[^\[\]]|\[[^\[\]]*\])*\])+)/;

function dmDims(sub) {
    const out = [];
    let d = 0, cur = [];
    for (const ch of sub) {
        if (ch === "[") { if (d === 0) cur = []; else cur.push(ch); d++; }
        else if (ch === "]") { d--; if (d === 0) out.push(pyTrim(cur.join(""))); else cur.push(ch); }
        else if (d > 0) cur.push(ch);
    }
    return out;
}

const dmHas = (v, e) => search(RX(String.raw`(?<![\w.>])` + reEscape(v) + String.raw`\b(?!\s*(?:->|\.|\[|\())`), e) !== null;
const dmMult = (v, e) => search(RX(String.raw`(?<![\w.>])` + reEscape(v) + String.raw`\s*\)?\s*(?:\*|<<)|\*\s*\(?\s*` + reEscape(v) + String.raw`\b`), e) !== null;

function dmStrideClass(v, dims) {
    const hit = dims.map((e, k) => (dmHas(v, e) ? k : -1)).filter(k => k >= 0);
    if (!hit.length) return null;
    if (hit.some(k => k < dims.length - 1)) return "strided";
    return dmMult(v, dims[dims.length - 1]) ? "strided" : "unit";
}

function dmForIvs(header) {
    const parts = header.split(";");
    if (parts.length !== 3) return new Set();
    const out = new Set();
    for (const m of fiter(/(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|[-+]=)|(?:\+\+|--)\s*([A-Za-z_]\w*)/, parts[2])) out.add(m[1] || m[2]);
    return out;
}

function dmBound(header, defs) {
    const parts = header.split(";");
    if (parts.length !== 3) return null;
    const m = search(/[<>]=?\s*\(?\s*([A-Za-z_]\w*|\d+)\s*\)?\s*$/, pyTrim(parts[1]));
    return m ? evalDim(m[1], defs) : null;
}

function dmBody(text) {
    const p = text.indexOf("(");
    let d = 0;
    for (let j = p; j < text.length; j++) {
        const ch = text.at(j);
        if (ch === "(") d++;
        else if (ch === ")") {
            d--;
            if (d === 0) {
                const rest = pyTrim(text.slice(j + 1));
                if (rest.startsWith("{") && rest.endsWith("}")) return rest.slice(1, -1);
                return rest;
            }
        }
    }
    return "";
}

const noWs = (s) => s.replace(/[ \n\t]/g, "");

function dmNest(inner, parent, defs) {
    let js = dmForIvs(inner.rawheader), is = dmForIvs(parent.rawheader);
    if (js.size !== 1 || is.size !== 1 || inner.kw !== "for" || parent.kw !== "for") return null;
    const j = [...js][0], i = [...is][0];
    if (i === j) return null;
    const bi = dmBound(inner.rawheader, defs), bo = dmBound(parent.rawheader, defs);
    if (bi !== null && bo !== null && bi <= 8 && bo <= 8) return null;
    if (bi !== null && bi <= 16) return null;
    const ph = parent.rawheader.split(";").map(pyTrim);
    if (ph.length === 3) {
        const bm = matchAt(/\w+\s*<=?\s*(.+)$/, ph[1]);
        const st = ph[0].replace(/^.*?=/, "");
        if (bm && search(RX(String.raw`(?<![\w.>])` + reEscape(pyTrim(bm[1])) + String.raw`(?![\w(])`), st)) return null;
    }
    const body = dmBody(inner.body);
    if (search(/\[\s*\w+\s*(?:\+\+|--)\s*\]|\[\s*(?:\+\+|--)\s*\w+\s*\]/, body)) return null;
    const fixes = [], breaks = [];
    for (const m of fiter(DM_ACC_RE, body)) {
        const dims = dmDims(m[2]);
        const cj = dmStrideClass(j, dims), ci = dmStrideClass(i, dims);
        const name = norm(m[1]) + dims.map(d => "[" + norm(d) + "]").join("");
        if (cj === "strided" && ci === "unit") fixes.push(name);
        else if (cj === "unit" && ci === "strided") breaks.push(name);
    }
    if (!fixes.length) return null;
    const pb = pyTrim(dmBody(parent.body));
    const perfect = noWs(pb) === noWs(pyTrim(inner.body));
    if (breaks.length) return ["cache-tiling", `inner ${j}: ${uniqSorted(fixes).slice(0, 2).join(",")} strided, ${uniqSorted(breaks).slice(0, 2).join(",")} unit; ` +
        `no loop order makes both unit-stride: block ${i}/${j}`];
    if (!perfect) return null;
    return ["loop-interchange", `${uniqSorted(fixes).slice(0, 3).join(",")} walk the slow dimension with inner ${j}: swap the ${i}/${j} loops`];
}

function dmVaries(base, L, iv) {
    if (iv && search(RX(String.raw`\[[^\]]*\b` + reEscape(iv) + String.raw`\b[^\]]*\]`), base)) return true;
    const root = matchAt(/\s*\(?\s*\*?\s*([A-Za-z_]\w*)/, base);
    if (!root) return false;
    const r = reEscape(root[1]);
    return !!search(RX(String.raw`(?<![\w.>])` + r + String.raw`\s*(?:=(?!=)|\+\+|--|\+=|-=)|(?:\+\+|--)\s*` + r + String.raw`\b`), L.body);
}

const dmClose = (s, i, o = "(", c = ")") => matchParen(s, i, o, c);

function dmKindDispatch(L, iv) {
    const body = dmBody(L.body);
    const hdr = norm(L.rawheader);
    if ((L.kw === "for" && fullmatch(/\s*;\s*;\s*/, L.rawheader)) || (L.kw === "while" && fullmatch(/\s*(?:1|true|TRUE)\s*/, hdr))) return null;
    for (const m of fiter(/(?<![\w.>])switch\s*\(/, body)) {
        const q = dmClose(body, mEnd(m) - 1);
        if (q < 0) continue;
        const subj = body.slice(mEnd(m), q);
        if (search(/\*\s*\w+\s*\+\+|\+\+\s*\w+/, subj)) continue;
        const fld = findall(/(?:->|\.)\s*([A-Za-z_]\w*)/, subj);
        if (!fld.length || !search(DM_TAG_RE, fld[fld.length - 1])) continue;
        const sb = search(/(?<![=!<>])=(?!=)/, subj) ? subj.replace(/^.*?=(?!=)/, "") : subj;
        if (!dmVaries(sb, L, iv)) continue;
        const ob = body.indexOf("{", q);
        const cb = ob >= 0 ? dmClose(body, ob, "{", "}") : -1;
        const ncase = cb > 0 ? findall(/\bcase\b/, pySlice(body, ob, cb)).length : 0;
        if (ncase >= 3) return `switch on ${norm(subj).slice(0, 40)} with ${ncase} cases per element: bucket or sort the elements by kind, then one tight loop per kind`;
    }
    const chain = new Map();
    for (const m of fiter(/([A-Za-z_][\w\[\]]*(?:(?:->|\.)\w+(?:\[[^\]]*\])*)*?)\s*(?:->|\.)\s*([A-Za-z_]\w*)\s*==\s*([A-Z_][A-Z0-9_]*)\b/, body)) {
        if (search(DM_TAG_RE, m[2]) && dmVaries(m[1], L, iv)) { if (!chain.has(m[2])) chain.set(m[2], new Set()); chain.get(m[2]).add(m[3]); }
    }
    for (const [f, vals] of chain) if (vals.size >= 3) return `if-chain on .${f} over ${vals.size} kinds per element: bucket or sort by kind first`;
    return null;
}

function dmFlagFilter(L, iv, ftext = "") {
    if (!iv || L.kw !== "for") return null;
    let body = pyTrim(dmBody(L.body));
    const ivr = reEscape(iv);
    let local = null, arr = null;
    const dm = matchAt(RX(String.raw`(?:const\s+)?(?:struct\s+)?\w+\s*\*?\s*(\w+)\s*=\s*&?\s*([A-Za-z_][\w.>\-]*)\s*\[\s*` + ivr + String.raw`\s*\]\s*;`), body);
    if (dm) { local = dm[1]; arr = dm[2]; body = pyTrim(body.slice(mEnd(dm))); }
    const m = matchAt(/if\s*\(/, body);
    if (!m) return null;
    const q = dmClose(body, mEnd(m) - 1);
    if (q < 0) return null;
    const cond = norm(body.slice(mEnd(m), q));
    const after = pyTrim(body.slice(q + 1));
    const isSkip = !!matchAt(/continue\s*;/, after);
    if (!isSkip) {
        if (!after.startsWith("{")) return null;
        const cb = dmClose(after, 0, "{", "}");
        const restAfterIf = cb > 0 ? pyTrim(after.slice(cb + 1)) : "x";
        if (restAfterIf) return null;
    }
    let pat, where;
    if (local) {
        pat = String.raw`(!\s*)?\(?\s*` + reEscape(local) + String.raw`\s*(?:->|\.)\s*(\w+)\s*\)?(\s*(==|!=)\s*(0|NULL|false|true))?`;
        where = arr + "[]";
    } else {
        pat = String.raw`(!\s*)?\(?\s*([A-Za-z_][\w.>\-]*)\s*\[\s*` + ivr + String.raw`\s*\]\s*(?:(?:->|\.)\s*(\w+))?\s*\)?(\s*(==|!=)\s*(0|NULL|false|true))?`;
        where = null;
    }
    const cm = fullmatch(RX(pat), cond);
    if (!cm) return null;
    let field;
    if (local) field = g(cm, 2);
    else { where = cm[2] + "[]"; field = g(cm, 3); }
    if (field && !search(DM_FLAG_RE, field) && !g(cm, local ? 3 : 4)) return null;
    if (!field) {
        const wb = where.slice(0, -2);
        const bn = reEscape(wb.split("->").at(-1).split(".").at(-1));
        const decl = search(RX(String.raw`([A-Za-z_][\w\s]*?)(\*+)?\s*\b` + bn + String.raw`\s*(\[)?`), ftext || "");
        const ptrs = decl ? (g(decl, 2) || "").length + (g(decl, 3) ? 1 : 0) : 0;
        const tyn = decl && words(decl[1]).length ? words(decl[1]).at(-1) : "";
        if (ptrs < 2 && tyn !== "JSValue" && !search(RX(reEscape(wb) + String.raw`\s*\[\s*` + ivr + String.raw`\s*\]\s*->`), body)) return null;
    }
    const what = field ? `.${field}` : " (empty slot)";
    const shape = isSkip ? "skip" : "guard";
    return `${shape} on ${where}${what} per element: keep a dense array or index list of the live entries and loop over that`;
}

const LUT_MEMO = new WeakMap();
function dmLutSize(base, text, defs) {
    let memo = LUT_MEMO.get(defs);
    if (!memo || memo.text !== text) { memo = { text, sizes: new Map() }; LUT_MEMO.set(defs, memo); }
    if (!memo.sizes.has(base)) memo.sizes.set(base, dmLutSizeRaw(base, text, defs));
    return memo.sizes.get(base);
}
function dmLutSizeRaw(base, text, defs) {
    const m = search(RX(String.raw`(?:^|[;{}\n])\s*(?:static\s+)?(?:const\s+)?(?:unsigned\s+|signed\s+)?[A-Za-z_]\w*(?:\s+const)?\s+` + reEscape(base) +
        String.raw`\s*((?:\[[^\]]*\])+)\s*(?:=|;)`), text);
    if (!m) return null;
    let total = 1;
    for (const d of findall(/\[([^\]]*)\]/, m[1])) {
        const v = pyTrim(d) ? evalDim(d, defs) : null;
        if (v === null) return null;
        total *= v;
    }
    return total;
}

const pyInt0 = (s) => {
    s = pyTrim(s);
    if (/^0[xX][0-9a-fA-F]+$/.test(s)) return parseInt(s.slice(2), 16);
    if (/^(?:0+|[1-9]\d*)$/.test(s)) return Number(s);
    throw new Error("invalid literal for int() with base 0: '" + s + "'");
};

function dmRandomGather(L, iv, ftext, text, defs) {
    if (!iv) return null;
    const body = dmBody(L.body);
    const ivr = reEscape(iv);
    const hits = [];
    const idxOf = new Map();
    for (const m of fiter(RX(String.raw`(?<![\w.>])(?:const\s+)?(?:\w+\s+)?(\w+)\s*=\s*(?:\(\s*\w+\s*\)\s*)?([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*` + ivr +
        String.raw`\s*(?:[-+]\s*\d+\s*)?\]\s*(&\s*(?:0x[0-9a-fA-F]+|\d+))?\s*;`), body)) idxOf.set(m[1], [norm(m[2]), g(m, 3)]);
    for (const m of fiter(RX(String.raw`(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*(?:\(\s*\w+\s*\)\s*)?([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*` + ivr +
        String.raw`\s*(?:[-+]\s*\d+\s*)?\]\s*(&\s*(?:0x[0-9a-fA-F]+|\d+))?\s*\]`), body)) hits.push([norm(m[1]), norm(m[2]), g(m, 3), mEnd(m)]);
    for (const [k, [src, mask]] of idxOf) {
        for (const m of fiter(RX(String.raw`(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*` + reEscape(k) + String.raw`\s*\]`), body)) hits.push([norm(m[1]), src, mask, mEnd(m)]);
    }
    const out = [];
    for (const [base, idx, mask, end] of hits) {
        if (base === idx) continue;
        if (mask) { const mv = pyInt0(mask.replace(/&/g, "")); if (mv < 4096) continue; }
        const lut = dmLutSize(base.split("->").at(-1).split(".").at(-1), text, defs);
        if (lut !== null && lut <= 4096) continue;
        const root = idx.split("->").at(-1).split(".").at(-1);
        if (search(RX(String.raw`\b(?:unsigned\s+char|uint8_t|int8_t|signed\s+char|char|u8|BYTE)\s*(?:const\s*)?\*\s*(?:const\s+)?(?:restrict\s+)?` + reEscape(root) + String.raw`\b`), ftext) ||
            search(RX(String.raw`\b(?:unsigned\s+char|uint8_t|int8_t|char|u8)\s+` + reEscape(root) + String.raw`\s*\[`), ftext)) continue;
        const rest = body.slice(end, end + 12);
        const kind = matchAt(/\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|\s*(?:\+\+|--)/, rest) ? "scatter" : "gather";
        out.push(`${kind} ${base}[${idx}[${iv}]]`);
    }
    if (!out.length) return null;
    return uniqSorted(out).slice(0, 3);
}

function dmLoopArrays(body) {
    const reads = new Map(), writes = new Map();
    for (const m of fiter(DM_ACC_RE, body)) {
        const base = norm(m[1]);
        const dims = dmDims(m[2]).map(norm).join("\x01");
        const rest = body.slice(mEnd(m), mEnd(m) + 12);
        const tgt = matchAt(/\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|\s*(?:\+\+|--)/, rest) ? writes : reads;
        if (!tgt.has(base)) tgt.set(base, new Set());
        tgt.get(base).add(dims);
    }
    return [reads, writes];
}

function dmFusion(loops, s) {
    const out = new Map();
    const fl = sortedBy(loops.filter(L => L.kw === "for" && L.inner), L => L.start);
    for (let k = 0; k + 1 < fl.length; k++) {
        const a = fl[k], b = fl[k + 1];
        if (a.func !== b.func || a.depth !== b.depth || a.stop >= b.start) continue;
        const gap = s.slice(a.stop + 1, b.start);
        if (pyTrim(pyStrip(pyTrim(gap), ";"))) continue;
        const ha = a.rawheader.replace(/\s+/g, ""), hb = b.rawheader.replace(/\s+/g, "");
        if (ha !== hb || ha.startsWith(";")) continue;
        const ivs = dmForIvs(a.rawheader);
        if (ivs.size !== 1) continue;
        const iv = [...ivs][0];
        const ba = dmBody(a.body), bb = dmBody(b.body);
        if (search(FIS_EXIT_RE, ba) || search(FIS_EXIT_RE, bb)) continue;
        const [ra, wa] = dmLoopArrays(ba), [rb, wb] = dmLoopArrays(bb);
        let bad = false;
        for (const [w, r] of [[wa, rb], [wb, ra], [wa, wb]]) {
            for (const [base, ixs] of w) {
                const other = r.get(base) || new Set();
                if (other.size && [...union(ixs, other)].some(x => x !== iv)) bad = true;
            }
        }
        const sa = minus(assignedNames(ba), new Set([iv])), sb = minus(assignedNames(bb), new Set([iv]));
        const ida = setOf(findall(/(?<![\w.>])([A-Za-z_]\w*)/, ba)), idb = setOf(findall(/(?<![\w.>])([A-Za-z_]\w*)/, bb));
        const scalA = setOf([...sa].filter(x => !wa.has(x))), scalB = setOf([...sb].filter(x => !wb.has(x)));
        const declA = setOf(findall(/(?<![\w.>])(?:[A-Za-z_]\w*)\s+\**\s*([A-Za-z_]\w*)\s*=/, ba));
        const declB = setOf(findall(/(?<![\w.>])(?:[A-Za-z_]\w*)\s+\**\s*([A-Za-z_]\w*)\s*=/, bb));
        if (inter(minus(scalA, declA), idb).length || inter(minus(scalB, declB), ida).length) bad = true;
        if (bad) continue;
        const shared = inter(union(ra.keys(), wa.keys()), union(rb.keys(), wb.keys())).sort();
        if (!shared.length) continue;
        out.set(a, `next loop (line ${b.line}) has the same header and shares ${shared.slice(0, 4).join(",")}: fuse into one pass`);
    }
    return out;
}

function pyEvalExt(ex) {
    const t = ex.replace(/0[xX][0-9a-fA-F]+|0[bB][01]+/g, (h) => String(BigInt(h)));
    return pyEval(t, true);
}

function dmEnums(s, defs) {
    const out = new Map();
    for (const m of fiter(/\benum\s+(\w+)?\s*\{([^{}]*)\}\s*(\w+)?/, s)) {
        const vals = new Map();
        let cur = -1, ok = true;
        for (let part of m[2].split(",")) {
            part = pyTrim(part);
            if (!part) continue;
            const em = matchAt(/([A-Za-z_]\w*)\s*(?:=\s*(.+))?$/s, part);
            if (!em) { ok = false; break; }
            if (em[2]) {
                const ex = em[2].replace(/\b([A-Za-z_]\w*)\b/g, (_, x) => (vals.has(x) ? String(vals.get(x)) : x));
                let v = evalDim(ex, defs);
                if (v === null) v = fullmatch(/[\dxXa-fA-F\s+\-*()<|&~]+/, ex) ? pyEvalExt(ex) : null;
                if (v === null) { ok = false; break; }
                cur = v;
            } else cur += 1;
            vals.set(em[1], cur);
        }
        if (!ok || !vals.size) continue;
        const vs = [...vals.values()];
        const lo = Math.min(...vs), hi = Math.max(...vs);
        for (const nm of [g(m, 1), g(m, 3)]) if (nm) out.set(nm, [lo, hi]);
    }
    return out;
}

const gcd = (a, b) => { while (b) [a, b] = [b, a % b]; return Math.abs(a); };
function dmStraddle(size) {
    if (size < 16 || size % 64 === 0 || 64 % size === 0 || size > 256) return null;
    const period = 64 / gcd(size, 64);
    const need = Math.floor((size + 63) / 64);
    let extra = 0;
    for (let k = 0; k < period; k++) { const st = (k * size) % 64; if (Math.floor((st + size + 63) / 64) > need) extra++; }
    return extra / period;
}

function matchBack(s, i, c, o) {
    if (i < 0) return -1;
    let d = 0, pc = s.lastIndexOf(c, i), po = s.lastIndexOf(o, i);
    while (po >= 0) {
        if (pc > po) { d++; pc = pc > 0 ? s.lastIndexOf(c, pc - 1) : -1; }
        else { d--; if (d === 0) return po; po = po > 0 ? s.lastIndexOf(o, po - 1) : -1; }
    }
    return -1;
}

const isRecursion = (e) => e instanceof RangeError || /stack overflow|recursion/i.test(String(e && e.message || e));

async function scanFile(path, defs) {
    const raw = readText(path);
    const s = blankCommentsAndStrings(raw);
    const rel = relpath(path, ROOT);
    const lineOf = new Int32Array(s.length + 1);
    let ln = 1;
    for (let p = 0, nl; ; ln++) {
        nl = s.indexOf("\n", p);
        if (nl < 0) { lineOf.fill(ln, p, s.length + 1); break; }
        lineOf.fill(ln, p, nl + 1);
        p = nl + 1;
    }
    const funcs = [];
    let depth = 0, i = 0;
    let nextO = s.indexOf("{"), nextC = s.indexOf("}");
    for (;;) {
        if (nextO < 0 && nextC < 0) break;
        i = nextO >= 0 && (nextC < 0 || nextO < nextC) ? nextO : nextC;
        if (i === nextO) {
            if (depth === 0) {
                const end = matchParen(s, i, "{", "}");
                let k = i - 1;
                while (k >= 0 && " \t\n".includes(s[k])) k--;
                let pre = s.slice(Math.max(0, k - 400), k + 1);
                pre = pre.replace(/__attribute__\s*\(\(.*?\)\)\s*$/, "");
                if (end > 0 && rstripWs(pre).endsWith(")")) {
                    const close = rstripWs(pre).length - 1;
                    const op = matchBack(pre, close, ")", "(");
                    const head = op >= 0 ? pre.slice(0, op) : "";
                    const m = search(/([A-Za-z_]\w*)\s*$/, head);
                    if (m && !KEYWORDS.has(m[1])) funcs.push([i, end, m[1]]);
                }
                if (end > 0) { nextO = s.indexOf("{", end + 1); nextC = s.indexOf("}", end + 1); continue; }
            }
            depth++;
            nextO = s.indexOf("{", i + 1);
        } else { depth--; nextC = s.indexOf("}", i + 1); }
    }
    const cs = callSites(s);
    const funcAt = (pos) => { for (const [a, b, name] of funcs) if (a <= pos && pos <= b) return [name, a, b]; return ["?", -1, -1]; };
    const loops = [];
    let fisPure = null;
    const fisPureLazy = { has: (nm) => { if (fisPure === null) fisPure = { extra: new Set(PROJECT.pure || []), pure: fisPureOracle(s, funcs) }; return fisPure.extra.has(nm) || fisPure.pure(nm); } };
    for (const m of fiter(LOOP_RE, s)) {
        const kw = m[1];
        if (kw === "while") {
            const before = trimEndAt(s, m.index);
            if (before > 0 && s[before - 1] === "}") {
                const openPos = matchBack(s, before - 1, "}", "{");
                if (openPos >= 0) {
                    const pre = trimEndAt(s, openPos);
                    if (pre >= 2 && s[pre - 2] === "d" && s[pre - 1] === "o") continue;
                }
            }
        }
        let header, e;
        if (kw === "for" || kw === "while") {
            const p = s.indexOf("(", mEnd(m));
            if (p < 0 || pyTrim(s.slice(mEnd(m), p))) continue;
            const q = matchParen(s, p, "(", ")");
            if (q < 0) continue;
            header = s.slice(p + 1, q);
            e = statementEnd(s, q + 1);
        } else {
            e = statementEnd(s, mEnd(m));
            header = "";
            if (e > 0) {
                const w = matchAt(/\s*while\s*\(/, s, e + 1);
                if (w) {
                    const p = mEnd(w) - 1;
                    const q = matchParen(s, p, "(", ")");
                    header = q > 0 ? s.slice(p + 1, q) : "";
                }
            }
        }
        if (e < 0) continue;
        const [fname, fa, fb] = funcAt(m.index);
        const body = s.slice(m.index, e + 1);
        loops.push({ file: rel, line: lineOf[m.index], end: lineOf.at(e), kw, func: fname, fa, fb, start: m.index, stop: e,
            rawheader: header, header: norm(header).slice(0, 90), body });
    }
    const nest = () => {
        for (const L of loops) {
            let dc = 0, inner = true;
            for (const o of loops) {
                if (o === L) continue;
                if (o.start < L.start && L.stop <= o.stop) dc++;
                if (L.start < o.start && o.stop <= L.stop) inner = false;
            }
            L.depth = 1 + dc;
            L.inner = inner;
        }
    };
    nest();
    const fused = dmFusion(loops, s);
    for (const L of loops) {
        const more = {};
        if (fused.has(L)) more["loop-fusion"] = fused.get(L);
        if (L.inner) {
            const ivs = L.kw === "for" ? dmForIvs(L.rawheader) : new Set();
            const iv = ivs.size === 1 ? [...ivs][0] : null;
            const par = loops.filter(o => o !== L && o.start < L.start && L.stop <= o.stop && o.depth === L.depth - 1);
            if (par.length) { const r = dmNest(L, par[0], defs); if (r) more[r[0]] = r[1]; }
            let r = dmKindDispatch(L, iv);
            if (r) more["kind-dispatch"] = r;
            const fpre = L.fa >= 0 ? s.slice(Math.max(0, rfind(s, "}", 0, L.fa) + 1), L.start) : "";
            r = dmFlagFilter(L, iv, fpre);
            if (r) more["flag-filter"] = r;
            if (iv && L.fa >= 0) {
                const ftext = fpre;
                r = dmRandomGather(L, iv, ftext, s, defs);
                if (r) more["random-gather"] = r.join(", ");
                const xc = new Set();
                const lb = dmBody(L.body);
                for (const cm of fiter(/(?<![\w.>])([A-Za-z_]\w*)\s*\(/, lb)) {
                    const nm = cm[1];
                    if (KEYWORDS.has(nm) || DM_KNOWN_EXT.has(nm) || fullmatch(/[A-Z][A-Z0-9_]*/, nm) || fullmatch(FIS_INTRIN_PURE, nm)
                        || FIS_PURE_VEC.has(nm) || FIS_PURE_SCALAR.has(nm) || nm.startsWith("__builtin_")) continue;
                    const q = matchParen(lb, mEnd(cm) - 1, "(", ")");
                    if (q > 0 && search(RX(String.raw`\[[^\]]*\b` + reEscape(iv) + String.raw`\b[^\]]*\]`), lb.slice(mEnd(cm), q))) xc.add(nm);
                }
                if (xc.size) L.xcalls = [...xc].sort();
            }
        }
        if (Object.keys(more).length) L.more = more;
    }
    nest();
    for (const L of loops) {
        const b = L.body;
        const hb = b.indexOf("{");
        const innerBody = hb >= 0 ? b.slice(hb) : b;
        const lo = L.start, hi = L.stop + 1;
        L.alloc = uniqSorted(csWords(cs, lo, hi, F_ALLOC));
        L.free = uniqSorted(csWords(cs, lo, hi, F_FREE));
        L.push = uniqSorted(csWords(cs, lo, hi, F_PUSH));
        L.smallcpy = csAny(cs, lo, hi, F_MEMX) ? uniqSorted(flist(SMALLCPY_RE, b).map(x => x[1] + "(" + x[3].replace(/ /g, "") + ")")) : [];
        L.libm = uniqSorted(csWords(cs, lo, hi, F_LIBM));
        L.fnptr = flist(FNPTR_RE, b).filter(x => !matchAt(/\w+\s*\[/, x[0]) || x[0].includes("(")).length;
        L.div = innerBody.includes("/") ? flist(DIV_RE, innerBody).length : 0;
        L.jsapi = csHits(cs, lo, hi, F_JSAPI).length;
        L.lines = L.end - L.line + 1;
        L.gist = gistLoopChecks(L, innerBody, L.rawheader, defs, cs, L.start + (hb >= 0 ? hb : 0));
        if (L._aos_all && L._aos_all.size && L.fa >= 0) {
            const ftext = s.slice(Math.max(0, L.fa - 600), L.start);
            const typed = new Map();
            for (const base of L._aos_all.keys()) {
                const tm = search(RX(String.raw`\b(?:struct\s+)?([A-Za-z_]\w*)\s*\*\s*(?:const\s+)?(?:restrict\s+)?` + reEscape(base) + String.raw`\b`), ftext) ||
                    search(RX(String.raw`\b(?:struct\s+)?([A-Za-z_]\w*)\s+` + reEscape(base) + String.raw`\s*\[`), ftext);
                if (tm && !KEYWORDS.has(tm[1])) typed.set(base, tm[1]);
            }
            L._aos_all_type = typed;
            L._aos_type = new Map([...typed].filter(([k]) => L._aos && L._aos.has(k)));
        }
        if (L.inner && L.fa >= 0) {
            const ftext = s.slice(L.fa, L.start);
            const red = setOf(findall(/(?<![\w.>\]\)])([A-Za-z_]\w*)\s*(?:\+=|-=)/, innerBody));
            const fpv = [];
            for (const v of red) {
                const ve = reEscape(v);
                if (v === L.lv || !L.lv || search(RX(String.raw`\b` + ve + String.raw`\s*\[`), innerBody)) continue;
                if (!search(RX(String.raw`\b(?:double|float)\s+(?:[^;{}()]*?[,\s*])?` + ve + String.raw`\s*[=;,]`), ftext)) continue;
                const upd = flist(RX(String.raw`(?<![\w.>])` + ve + String.raw`\s*(?:\+=|-=)\s*([^;]*);`), innerBody);
                const uses = flist(RX(String.raw`(?<![\w.>])` + ve + String.raw`\b`), innerBody).length;
                if (!upd.length || uses !== upd.length || search(RX(String.raw`\b(?:double|float|[A-Za-z_]\w*_t)\s+` + ve + String.raw`\b`), innerBody)) continue;
                if (!upd.some(u => search(RX(String.raw`\[[^\]]*\b` + reEscape(L.lv) + String.raw`\b`), u[1]))) continue;
                fpv.push(v);
            }
            const unrolled = search(/\+=\s*(?:[2-9]|\d\d+|[A-Z_][A-Z0-9_]*)\b/, L.header.split(";").at(-1));
            if (fpv.length && !unrolled) L.gist["fp-serial-reduction"] = fpv.sort();
        }
        if (L.inner) {
            let fr = null;
            try {
                const cm = L.kw === "for" && L.rawheader.split(";").length - 1 === 2 ? search(/[<>]=?\s*\(?\s*([A-Za-z_]\w*|\d+)\s*\)?\s*$/, L.rawheader.split(";")[1]) : null;
                const trip = cm ? evalDim(cm[1], defs) : null;
                fr = trip === null || trip > 16 ? await fission(L.kw, L.rawheader, L.body, fisPureLazy) : null;
            } catch (e) {
                if (!isRecursion(e)) throw e;
                fr = null;
            }
            if (fr) L.fission = fissionSummary(fr);
        }
        L.noinit = L.kw === "for" && lstripWs(L.rawheader).startsWith(";");
        delete L.body; delete L.rawheader; delete L.fb;
    }
    for (const L of loops) {
        if (L.noinit && L.lv && loops.some(o => o !== L && o.func === L.func && o.lv === L.lv && o.start < L.start && o.stop < L.start)) {
            L.tail = true;
            delete L.gist["single-vector-accumulator"];
            delete L.gist["fp-serial-reduction"];
        } else L.tail = false;
    }
    const arrays = [], fnotes = [];
    const fileHas = { mmap: !!search(/(?<![\w.>])mmap\s*\(/, s), madvise: !!search(/\b(?:posix_)?madvise\s*\(/, s) };
    for (const [a, b, name] of funcs) {
        const body = s.slice(a, b);
        for (const mm of localArrays(body)) {
            const l0 = lstripWs(mm[0]);
            if (l0.startsWith("static") || l0.startsWith("return") || l0.startsWith("case")) continue;
            const ty = pyTrim(mm[1]);
            if (["return", "goto", "else", "case", "sizeof"].includes(words(ty).at(-1))) continue;
            const dims = findall(/\[\s*([^\]\[]+?)\s*\]/, mm[3]);
            let total = 1, ok = true;
            for (const dsz of dims) { const v = evalDim(dsz, defs); if (v === null) { ok = false; break; } total *= v; }
            if (!ok) continue;
            const tkey = pyTrim(ty.split("const ").join(""));
            let esz = ty.includes("*") ? 8 : (tkey in TYPE_SIZE ? TYPE_SIZE[tkey] : null);
            const exact = esz !== null;
            if (esz === null) { const last = words(ty).at(-1); esz = last in TYPE_SIZE ? TYPE_SIZE[last] : 8; }
            const nbytes = total * esz;
            if (nbytes >= 256 && !mm[0].includes("DYN_IO_SCRATCH"))
                arrays.push({ file: rel, line: lineOf[a + mm.index], func: name, decl: norm(mm[0]).slice(0, 100), bytes: nbytes, exact, type: tkey });
        }
        if (csAny(cs, a, b, F_MEMSET)) for (const mm of fiter(MEMSET_RE, body)) {
            const sz = mm[2];
            let v = evalDim(sz, defs);
            if (v === null) {
                const sm = fullmatch(/sizeof\s*\(?\s*([A-Za-z_]\w*)\s*\)?/, pyTrim(sz));
                if (sm) { const hit = arrays.find(x => x.func === name && search(RX(String.raw`\b` + reEscape(sm[1]) + String.raw`\s*\[`), x.decl)); v = hit ? hit.bytes : null; }
            }
            if (v !== null && v >= 1024) fnotes.push({ check: "big-memset", file: rel, line: lineOf[a + mm.index], func: name, what: `memset ${v} B (${norm(sz).slice(0, 40)})` });
        }
        if (csAny(cs, a, b, F_CALLOCW)) for (const mm of fiter(CALLOC_RE, body)) {
            let v = null;
            const parts = mm[2].split(",").map(pyTrim);
            if (mm[1] === "calloc" && parts.length === 2) {
                const a1 = evalDim(parts[0], defs), a2 = evalDim(parts[1], defs);
                v = a1 !== null && a2 !== null ? a1 * a2 : null;
            }
            if (v !== null && v >= 1024) fnotes.push({ check: "big-memset", file: rel, line: lineOf[a + mm.index], func: name, what: `${mm[1]} ${v} B` });
        }
        for (const k of csHits(cs, a, b, F_SORT)) {
            const e = cs.end[k] - a;
            const q = matchParen(body, e - 1, "(", ")");
            const args = q > 0 ? norm(body.slice(e, q)).slice(0, 80) : "";
            fnotes.push({ check: "sort-call", file: rel, line: lineOf[cs.pos[k]], func: name, what: `${cs.word[k]}(${args})` });
        }
        const ws = counter();
        if (csAny(cs, a, b, F_WRITE)) for (const x of fiter(WRITE_RE, body)) cinc(ws, x[2]);
        for (const [fd, k] of ws) if (k >= 2) fnotes.push({ check: "write-sequence", file: rel, line: lineOf[a], func: name, what: `${k} write/send calls on ${fd}` });
        if (body.includes("atomic_") || body.includes("__ATOMIC_SEQ_CST") || body.includes("memory_order_seq_cst")) for (const mm of fiter(ATOMIC_DEFAULT_RE, body)) fnotes.push({ check: "seq-cst-atomic", file: rel, line: lineOf[a + mm.index], func: name, what: mm[1] || mm[0] });
        for (const k of csHits(cs, a, b, F_LISTWALK)) fnotes.push({ check: "macro-list-walk", file: rel, line: lineOf[cs.pos[k]], func: name, what: cs.word[k] });
    }
    for (const [a, b, name] of funcs) {
        const body = s.slice(a, b);
        if (body.includes("goto")) for (const lm of fiter(GOTO_LABEL_RE, body)) {
            const lab = lm[1];
            if (["default", "case", "public", "private"].includes(lab)) continue;
            const later = flist(RX(String.raw`\bgoto\s+` + reEscape(lab) + String.raw`\s*;`), body).filter(x => x.index > mEnd(lm));
            if (later.length) fnotes.push({ check: "goto-loop", file: rel, line: lineOf[a + lm.index], func: name, what: `label ${lab}: ${later.length} backward goto(s)`, end: lineOf[a + later.at(-1).index] });
        }
        const allocs = csWords(cs, a, b, F_ALLOC);
        if (allocs.length >= 4) fnotes.push({ check: "alloc-heavy-fn", file: rel, line: lineOf[a], func: name, what: `${allocs.length} allocation calls: ` + uniqSorted(allocs).join(",").slice(0, 80) });
        if (csAny(cs, a, b, F_ALLOCASSIGN)) for (const am of fiter(ALLOC_ASSIGN_RE, body)) {
            const v0 = am[1];
            const q = matchParen(body, mEnd(am) - 1, "(", ")");
            let arg = q > 0 ? norm(body.slice(mEnd(am), q)) : "";
            if (am[2].includes("realloc")) continue;
            const parts = arg.split(",").map(pyTrim);
            if (am[2].toLowerCase().includes("calloc") && parts.length === 2) arg = parts.join(" * ");
            else if (parts.length >= 2) arg = parts.at(-1);
            const cst = fullmatch(/(?:\(?\s*(?:\d+|[A-Z_][A-Z0-9_]*|sizeof\s*\(?[^()]*\)?)\s*\)?\s*[*+]?\s*)+/, arg || "x");
            const after = body.slice(mEnd(am));
            const leaf = reEscape(v0.split("->").at(-1).split(".").at(-1));
            const aOff = a + mEnd(am);
            const freed = csHits(cs, aOff, b, F_FREE).some(k => { const e = cs.end[k] - aOff; return search(RX(String.raw`\b` + leaf + String.raw`\s*\)`), after.slice(e, e + 200).split(";")[0]); });
            const short = reEscape(v0);
            const escapes = v0.includes("->") || v0.includes(".")
                || search(RX(String.raw`\breturn\b[^;]*\b` + short + String.raw`\b`), body)
                || search(RX(String.raw`=\s*(?:\([^()]*\)\s*)?` + short + String.raw`\s*[;,)]`), after)
                || flist(RX(String.raw`\b([A-Za-z_]\w*)\s*\([^;()]*\b` + short + String.raw`\b`), after)
                    .some(c => !(csMask(c[1]) & F_FREE) && !KEYWORDS.has(c[1]) && !["memcpy", "memset", "memmove"].includes(c[1]));
            if (cst && freed && !escapes) {
                const v = evalDim(arg.replace(/sizeof\s*\(?[^()]*\)?/g, "8"), defs);
                fnotes.push({ check: "local-heap-temp", file: rel, line: lineOf[a + am.index], func: name, what: `${v0} = ${am[2]}(${arg.slice(0, 50)}) freed locally` + (v ? `, ~${v} B` : "") });
            }
        }
        const colds = csHits(cs, a, b, F_COLD, true).filter(k => { const x = cs.pos[k] - a; return !body.slice(Math.max(0, x - 160), x).includes("unlikely"); });
        if (colds.length) fnotes.push({ check: "_cold_calls", file: rel, line: lineOf[a], func: name, what: String(colds.length) });
        const sl = counter();
        if (csAny(cs, a, b, F_STRLEN)) for (const x of fiter(/\bstrlen\s*\(\s*([A-Za-z_][\w.>\-]*)\s*\)/, body)) cinc(sl, norm(x[1]));
        for (const [v, k] of sl) {
            if (k >= 2 && !search(RX(String.raw`(?<![\w.>])` + reEscape(v) + String.raw`\s*(?:\[[^\]]*\]\s*)?(?:[-+]?=(?!=)|\+\+|--)`), body))
                fnotes.push({ check: "ptr-strlen-repeat", file: rel, line: lineOf[a], func: name, what: `strlen(${v}) x${k}` });
        }
        let head = s.slice(Math.max(0, a - 800), a);
        head = head.slice(Math.max(head.lastIndexOf(";"), head.lastIndexOf("}")) + 1);
        const hm = search(RX(String.raw`([A-Za-z_][\w\s\*]*?)\b` + reEscape(name) + String.raw`\s*\((.*)\)\s*$`, "s"), head);
        if (hm) {
            const ret = norm(hm[1]);
            const params = hm[2].split(",").map(norm);
            const byval = params.filter(p => !p.includes("*") && !p.includes("[") && matchAt(/(?:const\s+)?(?:struct\s+)?[A-Za-z_]\w*\s+[A-Za-z_]\w*$/, p));
            const rtype = pyTrim(ret.replace(/\b(static|inline|extern|const|__attribute__\s*\(\(.*?\)\))\b/g, ""));
            const rsplit1 = (p, sep) => { const i = p.lastIndexOf(sep); return i < 0 ? p : p.slice(0, i); };
            fnotes.push({ check: "_sig", file: rel, line: lineOf[a], func: name, what: "",
                byval: byval.map(p => pyTrim(rsplit1(p, " ").split("const ").join("").split("struct ").join(""))),
                ret: rtype.includes("*") ? "" : rtype,
                ptrs: params.filter(p => p.includes("*") && !p.includes("(*")).map(p => pyTrim(rsplit1(p, "*").replace(/\bconst\b|\brestrict\b|\s+/g, " ")) + (p.split("*")[0].includes("const") ? "|r" : "|w")),
                restrict: hm[2].includes("restrict") });
        }
        if (body.includes("fsync") || body.includes("fdatasync") || body.includes("F_FULLFSYNC")) for (const fm of fiter(FSYNC_RE, body)) fnotes.push({ check: "fsync-call", file: rel, line: lineOf[a + fm.index], func: name, what: fm[1] || "F_FULLFSYNC" });
    }
    if (search(/\bsocket\s*\([^;]*SOCK_STREAM/, s) && search(/\b(?:connect|accept4?|listen)\s*\(/, s) && !s.includes("TCP_NODELAY") && !s.includes("set_nodelay"))
        fnotes.push({ check: "tcp-nodelay-missing", file: rel, line: 1, func: "-", what: "SOCK_STREAM sockets without TCP_NODELAY" });
    if (fileHas.mmap && !fileHas.madvise) fnotes.push({ check: "mmap-no-madvise", file: rel, line: 1, func: "-", what: "mmap without madvise" });
    const structs = parseStructs(s, lineOf, rel);
    const members = { __ranges__: funcs.map(([a, b, name]) => [rel, lineOf[a], lineOf[b], name]), __enums__: dmEnums(s, defs), __extern__: new Map(), __types__: new Map() };
    if (rel.endsWith(".c") && !rel.endsWith(".inc.c")) {
        for (const [a, , name] of funcs) {
            const sig = Math.max(rfind(s, "}", 0, a), rfind(s, ";", 0, a)) + 1;
            if (!search(/\bstatic\b/, s.slice(sig, a))) members.__extern__.set(name, rel);
        }
    }
    for (const [a, b, name] of funcs) {
        if (search(/(?:->|\.)\s*([A-Za-z_]\w*)\b(?!\s*\()/, s.slice(a, b))) {
            const sig = Math.max(rfind(s, "}", 0, a), rfind(s, ";", 0, a)) + 1;
            const text = s.slice(sig, b);
            const ams = flist(/\b([A-Za-z_]\w*)\s*(->|\.)\s*([A-Za-z_]\w*)\b(?!\s*\()/, text);
            const vt = declTypes(text, new Set(ams.map(am => am[1])));
            const typed = counter();
            for (const am of ams) {
                const t = vt.get(am[1]);
                if (t && t[1] === (am[2] === "->")) cinc(typed, t[0] + "\x00" + am[3]);
            }
            members.__types__.set(name, typed);
        }
    }
    return [loops, arrays, fnotes, structs, members];
}

const DECL_RE = /\b(?:struct\s+|union\s+)?([A-Za-z_]\w*)\s*(\*+)?\s*(?:const\s+|restrict\s+)*([A-Za-z_]\w*)\s*(?=[,;=)\[])/g;
const ENDW_RE = /\w+(?=\s*[,;=)\[])/g;
function declTypes(text, need) {
    const vt = new Map();
    if (!need.size) return vt;
    ENDW_RE.lastIndex = 0;
    let m;
    while ((m = ENDW_RE.exec(text)) !== null) {
        const w = m[0];
        let hit = false;
        for (const x of need) if (x.length <= w.length && w.endsWith(x)) { hit = true; break; }
        if (!hit) continue;
        let e = m.index + w.length;
        while (isJsSpace(text.charCodeAt(e))) e++;
        let q = m.index;
        for (;;) {
            if (q <= 0) break;
            const c = text.charCodeAt(q - 1);
            if (!(isWordCode(c) || c === 42 || isJsSpace(c))) break;
            q--;
        }
        DECL_RE.lastIndex = 0;
        const dm = DECL_RE.exec(text.slice(q, e + 1));
        if (dm !== null && !KEYWORDS.has(dm[1]) && !KEYWORDS.has(dm[3]) && !vt.has(dm[3])) vt.set(dm[3], [dm[1], !!dm[2]]);
    }
    return vt;
}

function normpath(p) {
    const abs = p.startsWith("/");
    const out = [];
    for (const x of p.split("/")) {
        if (x === "" || x === ".") continue;
        if (x === ".." && out.length && out.at(-1) !== "..") out.pop();
        else if (x === ".." && abs) continue;
        else out.push(x);
    }
    const r = (abs ? "/" : "") + out.join("/");
    return r || (abs ? "/" : ".");
}

function sourceRoots() {
    const roots = (PROJECT.roots && PROJECT.roots.length) ? PROJECT.roots : (isDir(join(ROOT, "src")) ? ["src"] : ["."]);
    return roots.map(r => join(ROOT, r));
}

function sourceFiles() {
    const exts = PROJECT.extensions || [".c", ".h"];
    const out = [];
    for (const top of sourceRoots()) walk(top, (d, _, fs) => { for (const f of fs) if (exts.some(e => f.endsWith(e))) out.push(join(d, f)); });
    return uniqSorted(out);
}

const run = (argv, opts = {}) => {
    const r = sys.Exec(argv[0], argv.slice(1), { maxBuffer: 1 << 30, ...opts });
    return { code: r.code === null ? -1 : r.code, stdout: r.stdout, stderr: r.stderr };
};

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    let len = 0; for (const p of parts) len += p.length;
    const all = new Uint8Array(len); let o = 0; for (const p of parts) { all.set(p, o); o += p.length; }
    return new TextDecoder().decode(all);
}

async function runAsync(argv, opts = {}) {
    const p = new sys.Spawn(argv[0], argv.slice(1), { stdin: "ignore", ...opts });
    const [stdout, stderr] = await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const r = await p.wait();
    p.close();
    return { code: r.code === null ? -1 : r.code, stdout, stderr };
}

const say = (s) => { std.out.puts(s + "\n"); std.out.flush(); };

function hardenFlags() {
    const h = PROJECT.harden;
    if (!h) return [];
    const flags = [...(h.flags || [])];
    let cc = "";
    try { cc = run(["clang", "--version"]).stdout.split("\n")[0] || ""; } catch (e) { cc = ""; }
    const want = words(cc).join("_");
    if (h.cache_glob) {
        for (const hf of file.glob(join(ROOT, h.cache_glob)).map(String).sort()) {
            const lines = readText(hf).split("\n");
            if (lines.at(-1) === "") lines.pop();
            if (lines.length >= 2 && lines[0].startsWith(want) && lines[1].startsWith("flags;")) {
                const got = lines[1].split(";").slice(1).filter(x => x && !x.startsWith("-W"));
                say(`hardening flags from ${relpath(hf, ROOT)}: ${got.join(" ")}`);
                return [...flags, ...got];
            }
        }
    }
    if (h.probe_file && h.probe_regex && exists(join(ROOT, h.probe_file))) {
        const m = search(new RegExp(h.probe_regex, "m"), readText(join(ROOT, h.probe_file)));
        if (m) {
            const probes = [...words(m[1]), ...(h["probe_extra_" + sys.uname().machine] || [])];
            for (const pr of probes) {
                if (pr.startsWith("-W")) continue;
                if (run(["clang", "-Werror", pr, "-x", "c", "-o", "/dev/null", "-"], { input: "int main(void){return 0;}" }).code === 0) flags.push(pr);
            }
            say(`hardening flags probed for ${cc}: ${flags.join(" ")}`);
        }
    }
    return flags;
}

function remarkCmds() {
    const harden = hardenFlags();
    const cmds = [];
    const ccj = join(ROOT, PROJECT.compile_commands || "compile_commands.json");
    if (exists(ccj)) {
        for (const e of JSON.parse(readText(ccj))) {
            const argv = e.arguments || shlexSplit(e.command);
            const src = normpath(join(e.directory || ROOT, e.file));
            cmds.push([src, argv, harden, e.directory || ROOT]);
        }
        say(`compile commands: ${relpath(ccj, ROOT)} (${cmds.length} entries)`);
        return cmds;
    }
    const hook = PROJECT.compile_hook;
    if (hook) {
        const out = run(["/bin/sh", "-c", hook], { cwd: ROOT }).stdout;
        for (const l of out.split("\n").filter((x, i, a) => i < a.length - 1 || x !== "")) {
            const argv = shlexSplit(l);
            if (!argv.includes("-c") || !argv.includes("-o")) continue;
            const src = argv.find(a => a.endsWith(".c"));
            if (src) cmds.push([src, argv, harden, ROOT]);
        }
        say(`compile commands: hook \`${hook}\` (${cmds.length} entries)`);
        return cmds;
    }
    const inc = [];
    for (const r of sourceRoots()) walk(r, (d) => inc.push("-I" + d));
    const cc = sys.getEnv("CC") || "clang";
    for (const f of sourceFiles()) if (f.endsWith(".c") && !f.endsWith(".inc.c")) cmds.push([f, [cc, "-O2", "-c", f, "-o", "/dev/null", ...inc], harden, ROOT]);
    say(`compile commands: none found; compiling ${cmds.length} sources with ${cc} -O2 and the source dirs as includes`);
    return cmds;
}

async function runRemark(job, outdir) {
    const [src, argv, harden, cwd] = job;
    const base = relpath(join(cwd, src), ROOT).replace(/[^\w.-]/g, "_");
    const yml = join(outdir, base + ".opt.yaml");
    const a = [];
    let skip = false;
    for (const x of argv) {
        if (skip) { skip = false; continue; }
        if (x === "-o" || x === "-MF") { skip = true; continue; }
        if (["-c", "-MMD", "-MD", "-g"].includes(x) || x.startsWith("-W") || x === "-w") continue;
        a.push(x);
    }
    const cmd = ["nice", "-n", "19", ...a, ...harden, "-S", "-o", "/dev/null", "-Wno-everything", "-Wpadded",
        "-Xclang", "-fdump-record-layouts-complete", "-fsave-optimization-record=yaml",
        "-foptimization-record-passes=" + REMARK_PASSES, "-foptimization-record-file=" + yml];
    const r = await runAsync(cmd, { cwd });
    writeText(join(outdir, base + ".padded"), r.stderr);
    writeText(join(outdir, base + ".layout"), r.stdout);
    const errs = r.stderr.split("\n").filter(l => l.includes(" error: "));
    return [src, r.code, errs.slice(-3).join("\n")];
}

const keyOf = (...a) => a.join("\x00");
const textLines = (t) => { const ls = t.split("\n"); if (ls.at(-1) === "") ls.pop(); return ls; };

const REMARK_ARGS = ["ClobberedBy", "Callee", "Cost", "Threshold", "NumSpills", "NumReloads", "TotalSpillsCost", "TotalReloadsCost", "VectorizationFactor", "InterleaveCount"];
const DOC_RE = /^--- !(Passed|Missed|Analysis)\s*$/;
const DEBUGLOC_RE = /DebugLoc:\s*\{\s*File:\s*'?([^',]+)'?,\s*Line:\s*(\d+)/y;
const PASS_RE = /Pass:\s*(\S+)/y, NAME_RE = /Name:\s*(\w+)/y, FUNC_RE = /Function:\s*'?([^'\s]+)'?/y;
const STRING_RE = /\s*-\s*String:\s*(.*?)\s*$/y, ARG_RE = /\s*-\s*(\w+):\s*'?([^']*?)'?\s*$/y;
const sticky = (re, s, i = 0) => { re.lastIndex = i; return re.exec(s); };

let REL_ROOT = null, REL = new Map();
function remarkRel(f) {
    if (REL_ROOT !== ROOT) { REL_ROOT = ROOT; REL = new Map(); }
    let r = REL.get(f);
    if (r === undefined) { r = normpath(f).split(ROOT + "/").join(""); REL.set(f, r); }
    return r;
}
function collectRemarksLines(text) {
    const out = [];
    let kind = null, cur = {};
    const done = () => {
        if (!(cur.file && kind)) return;
        const pas = cur.pass, name = cur.name;
        const keep = pas === "loop-vectorize" || (pas === "licm" && kind === "Missed") || (pas === "gvn" && kind === "Missed" && name === "LoadClobbered")
            || (pas === "slp-vectorizer" && cur.function) || (pas === "inline" && kind === "Missed" && (name === "TooCostly" || name === "NeverInline") && cur.function)
            || (pas === "regalloc" && name === "LoopSpillReloadCopies");
        if (!keep) return;
        const args = {};
        for (const k of REMARK_ARGS) if (k in cur.args) args[k] = cur.args[k];
        out.push({ kind, file: remarkRel(cur.file), line: cur.line, pass: pas, name, function: cur.function, args,
            text: pas === "loop-vectorize" ? pyTrim(cur.strings.join("")) : "" });
    };
    for (const line of textLines(text)) {
        const c0 = line.charCodeAt(0);
        if (c0 === 45 || c0 === 46) {
            const m = c0 === 45 ? DOC_RE.exec(line) : null;
            if (m || line.startsWith("...")) { done(); kind = m ? m[1] : null; cur = { strings: [], args: {} }; continue; }
        }
        if (kind === null) continue;
        let i = 0;
        while (i < line.length && (line.charCodeAt(i) === 32 || line.charCodeAt(i) === 9)) i++;
        const ci = line.charCodeAt(i);
        if (ci === 68 && !("file" in cur) && line.startsWith("DebugLoc:", i)) {
            const m = sticky(DEBUGLOC_RE, line, i);
            if (m && !("file" in cur)) { cur.file = m[1]; cur.line = Number(m[2]); continue; }
        }
        if (i === 0) {
            let m;
            if (c0 === 80 && (m = sticky(PASS_RE, line))) { cur.pass = m[1]; continue; }
            if (c0 === 78 && (m = sticky(NAME_RE, line))) { cur.name = m[1]; continue; }
            if (c0 === 70 && (m = sticky(FUNC_RE, line))) { cur.function = m[1]; continue; }
        }
        if (ci === 45) {
            let m = sticky(STRING_RE, line);
            if (m) {
                let v = m[1];
                if (v.length >= 2 && v[0] === v.at(-1) && (v[0] === "'" || v[0] === '"')) v = v.slice(1, -1);
                cur.strings.push(v);
                continue;
            }
            m = sticky(ARG_RE, line);
            if (m) cur.args[m[1]] = m[2];
        }
    }
    done();
    return out;
}

const RM_PATS = ["\n--- !", "\n...", "\nPass:", "\nName:", "\nFunction:", "DebugLoc:", ...REMARK_ARGS.map(k => k + ":")];
let RM_MATCHER = null;
const RK_KIND = ["Passed", "Missed", "Analysis"];
function remarkEncoder() {
    const S = [], ix = new Map(), R = [];
    const si = (v) => {
        if (v === undefined) return -1;
        let k = ix.get(v);
        if (k === undefined) { k = S.length; S.push(v); ix.set(v, k); }
        return k;
    };
    let last = -1;
    return {
        entry: [S, R],
        push(r) {
            const rec = [RK_KIND.indexOf(r.kind), si(r.file), r.line, si(r.pass), si(r.name), si(r.function), 0];
            REMARK_ARGS.forEach((k, b) => { if (k in r.args) { rec[6] |= 1 << b; rec.push(si(r.args[k])); } });
            rec.push(si(r.text));
            if (last >= 0 && R.length - last - 1 === rec.length && rec.every((v, k) => R[last + 1 + k] === v)) { R[last]++; return; }
            last = R.length;
            R.push(1, ...rec);
        },
    };
}
function remarkChunk(text, k, c) {
    const at = (q) => {
        if (q <= 0) return 0;
        if (q >= c) return text.length;
        let i = text.indexOf("\n--- !", Math.floor(q * text.length / c) - 1);
        while (i >= 0) {
            const e = text.indexOf("\n", i + 1);
            if (DOC_RE.test(text.slice(i + 1, e < 0 ? text.length : e))) return i + 1;
            i = text.indexOf("\n--- !", i + 1);
        }
        return text.length;
    };
    return text.slice(at(k), at(k + 1));
}
function mergeRemarks(parts) {
    if (parts.length === 1) return parts[0];
    const S = [], ix = new Map(), R = [];
    let last = -1;
    for (const [S2, R2] of parts) {
        const map = new Map();
        const si = (v) => {
            if (v < 0) return v;
            let k = map.get(v);
            if (k === undefined) { const str = S2[v]; k = ix.get(str); if (k === undefined) { k = S.length; S.push(str); ix.set(str, k); } map.set(v, k); }
            return k;
        };
        for (let i = 0; i < R2.length;) {
            const n = R2[i], mask = R2[i + 7];
            const rec = [R2[i + 1], si(R2[i + 2]), R2[i + 3], si(R2[i + 4]), si(R2[i + 5]), si(R2[i + 6]), mask];
            i += 8;
            for (let b = 0; b < REMARK_ARGS.length; b++) if (mask & (1 << b)) rec.push(si(R2[i++]));
            rec.push(si(R2[i++]));
            if (last >= 0 && R.length - last - 1 === rec.length && rec.every((v, k) => R[last + 1 + k] === v)) { R[last] += n; continue; }
            last = R.length;
            R.push(n, ...rec);
        }
    }
    return [S, R];
}
function collectRemarks(path, k = 0, c = 1) {
    const T = "\n" + (c === 1 ? readText(path) : remarkChunk(readText(path), k, c));
    const hits = (RM_MATCHER ??= new MultiMatcher(RM_PATS)).allIn(T);
    const enc = remarkEncoder();
    const out = { push: (r) => enc.push(r) };
    let kind = null, cur = null, docStart = 0;
    const lineAt = (ls) => { const e = T.indexOf("\n", ls); return T.slice(ls, e < 0 ? T.length : e); };
    const finish = (docEnd) => {
        if (kind === null || !cur.file) return;
        const pas = cur.pass, name = cur.name;
        if (pas === "loop-vectorize") { for (const r of collectRemarksLines(T.slice(docStart, docEnd))) out.push(r); return; }
        const keep = (pas === "licm" && kind === "Missed") || (pas === "gvn" && kind === "Missed" && name === "LoadClobbered")
            || (pas === "slp-vectorizer" && cur.function) || (pas === "inline" && kind === "Missed" && (name === "TooCostly" || name === "NeverInline") && cur.function)
            || (pas === "regalloc" && name === "LoopSpillReloadCopies");
        if (!keep) return;
        const args = {};
        for (const k of REMARK_ARGS) if (k in cur.args) args[k] = cur.args[k];
        out.push({ kind, file: remarkRel(cur.file), line: cur.line, pass: pas, name, function: cur.function, args, text: "" });
    };
    for (const h of hits) {
        const p = h.index, at = h.at;
        if (p <= 1) {
            const ls = at + 1;
            let k = null;
            if (p === 0) { const m = DOC_RE.exec(lineAt(ls)); if (!m) continue; k = m[1]; }
            finish(ls);
            kind = k; cur = { args: {} }; docStart = ls;
            continue;
        }
        if (kind === null) continue;
        if (p <= 4) {
            const m = sticky(p === 2 ? PASS_RE : p === 3 ? NAME_RE : FUNC_RE, lineAt(at + 1));
            if (m) { if (p === 2) cur.pass = m[1]; else if (p === 3) cur.name = m[1]; else cur.function = m[1]; }
            continue;
        }
        const ls = T.lastIndexOf("\n", at - 1) + 1;
        if (p === 5) {
            if ("file" in cur) continue;
            let j = ls;
            while (j < at && (T.charCodeAt(j) === 32 || T.charCodeAt(j) === 9)) j++;
            if (j !== at) continue;
            const m = sticky(DEBUGLOC_RE, lineAt(ls), at - ls);
            if (m) { cur.file = m[1]; cur.line = Number(m[2]); }
            continue;
        }
        let j = ls;
        while (T.charCodeAt(j) === 32 || T.charCodeAt(j) === 9) j++;
        if (T.charCodeAt(j) !== 45) continue;
        const line = lineAt(ls);
        if (sticky(STRING_RE, line)) continue;
        const m = sticky(ARG_RE, line);
        if (m && m[1] === REMARK_ARGS[p - 6]) cur.args[m[1]] = m[2];
    }
    finish(T.length);
    return enc.entry;
}

const RA_CLOBBERED = 0, RA_CALLEE = 1, RA_COST = 2, RA_THRESHOLD = 3, RA_NUMSPILLS = 4, RA_NUMRELOADS = 5, RA_SPILLSCOST = 6, RA_RELOADSCOST = 7, RA_VF = 8, RA_IC = 9;
const RA_POPCOUNT = Array.from({ length: 1 << REMARK_ARGS.length }, (_, m) => { let c = 0; for (; m; m &= m - 1) c++; return c; });
const raArg = (S, R, a0, mask, b) => (mask & (1 << b) ? S[R[a0 + RA_POPCOUNT[mask & ((1 << b) - 1)]]] : undefined);
function foldRemarks([S, R]) {
    const out = [], first = new Map();
    const missed = RK_KIND.indexOf("Missed");
    for (let i = 0; i < R.length;) {
        const len = 9 + RA_POPCOUNT[R[i + 7]];
        const pas = S[R[i + 4]];
        if (R[i + 1] === missed && (pas === "licm" || (pas === "gvn" && S[R[i + 5]] === "LoadClobbered"))) {
            const key = R.slice(i + 1, i + len).join(",");
            const at = first.get(key);
            if (at !== undefined) { out[at] += R[i]; i += len; continue; }
            first.set(key, out.length);
        }
        for (let k = 0; k < len; k++) out.push(R[i + k]);
        i += len;
    }
    return [S, out];
}
function applyRemarks(perFile) {
    const vec = new Map();
    const licm = [], gvn = [], spills = new Map();
    const extra = { slp_ok: new Multiset(), slp_missed: new Multiset(), inline_costly: [], vf: {} };
    for (const [S, R] of perFile) {
        for (let i = 0; i < R.length;) {
            const n = R[i++];
            const kind = RK_KIND[R[i]], kf = S[R[i + 1]], kl = R[i + 2], pas = S[R[i + 3]], name = S[R[i + 4]], fn = S[R[i + 5]], mask = R[i + 6];
            const a0 = i + 7;
            i = a0 + RA_POPCOUNT[mask];
            const text = S[R[i++]];
            if (pas === "loop-vectorize") {
                const args = {};
                if (raArg(S, R, a0, mask, RA_VF) !== undefined) args.VectorizationFactor = raArg(S, R, a0, mask, RA_VF);
                if (raArg(S, R, a0, mask, RA_IC) !== undefined) args.InterleaveCount = raArg(S, R, a0, mask, RA_IC);
                for (let k = 0; k < n; k++) flushVec(vec, kind, { name, args, text }, keyOf(kf, kl));
            }
            else if (pas === "licm" && kind === "Missed") licm.push([kf, kl, name, n]);
            else if (pas === "gvn" && kind === "Missed" && name === "LoadClobbered") gvn.push([kf, kl, raArg(S, R, a0, mask, RA_CLOBBERED) ?? "?", n]);
            else if (pas === "slp-vectorizer" && fn) (kind === "Passed" ? extra.slp_ok : extra.slp_missed).add(keyOf(kf, fn), n);
            else if (pas === "inline" && kind === "Missed" && (name === "TooCostly" || name === "NeverInline") && fn) {
                const e = [kf, kl, fn, raArg(S, R, a0, mask, RA_CALLEE) ?? "?", raArg(S, R, a0, mask, RA_COST) ?? "", raArg(S, R, a0, mask, RA_THRESHOLD) ?? "", name];
                for (let k = 0; k < n; k++) extra.inline_costly.push(k ? [...e] : e);
            }
            else if (pas === "regalloc" && name === "LoopSpillReloadCopies") {
                const k = keyOf(kf, kl);
                if (!spills.has(k)) spills.set(k, [0, 0, 0, 0.0]);
                const sp = spills.get(k);
                for (let c = 0; c < n; c++) {
                    sp[0] = Math.max(sp[0], pyInt(raArg(S, R, a0, mask, RA_NUMSPILLS) ?? 0));
                    sp[1] = Math.max(sp[1], pyInt(raArg(S, R, a0, mask, RA_NUMRELOADS) ?? 0));
                    sp[2] += 1;
                    sp[3] = Math.max(sp[3], pyFloat(raArg(S, R, a0, mask, RA_SPILLSCOST) || 0) + pyFloat(raArg(S, R, a0, mask, RA_RELOADSCOST) || 0));
                }
            }
        }
    }
    return [vec, licm, gvn, spills, extra];
}

const pyInt = (v) => { if (typeof v === "number") return v; const t = pyTrim(v); if (!/^[-+]?\d+$/.test(t)) throw new Error("invalid literal for int(): '" + v + "'"); return Number(t); };
const pyFloat = (v) => { if (typeof v === "number") return v; const t = pyTrim(v); const x = Number(t); if (t === "" || Number.isNaN(x) && !/^[-+]?nan$/i.test(t)) throw new Error("could not convert string to float: '" + v + "'"); return x; };

function flushVec(rec, kind, cur, key) {
    if (!rec.has(key)) rec.set(key, { vec: 0, missed: 0, reasons: new Multiset(), width: new Set() });
    const r = rec.get(key);
    const text = cur.text;
    if (kind === "Passed" && (cur.name === "Vectorized" || cur.name === "Interleaved")) {
        r.vec += 1;
        let vf = cur.args.VectorizationFactor, ic = cur.args.InterleaveCount;
        if (cur.name === "Interleaved") vf = "1";
        if (vf) r.width.add(`${vf}x${ic || 1}`);
    } else if (kind === "Missed") r.missed += 1;
    else if (kind === "Analysis" && text) {
        let reason = text.replace(/^loop not vectorized:\s*/, "");
        reason = ((cur.name ?? "?") + ": " + reason.replace(/\d+/g, "N")).slice(0, 120);
        r.reasons.add(reason);
    }
}

const statKey = (outdir, ext) => BLAKE3Hex([cacheVersion(), outdir, ...listDir(outdir).filter(f => f.endsWith(ext)).map(f => { const st = statOf(join(outdir, f)); return st ? `${f}:${st.size}:${st.mtimeMs}` : f + ":-"; })].join("\n"));
const parsePadded = (outdir) => new Map(resultCache("padded", statKey(outdir, ".padded"), () => [...parsePaddedRaw(outdir)]));
const parseLayouts = (outdir) => new Map(resultCache("layouts", statKey(outdir, ".layout"), () => [...parseLayoutsRaw(outdir)]));
function parsePaddedRaw(outdir) {
    const seen = new Set(), out = new Map();
    const pr = /^(.+?):(\d+):(\d+): warning: padding (struct|size of) '(?:struct |union )?([^']+)' with (\d+) bytes? to (align '([^']+)'|alignment boundary)/;
    for (const f of listDir(outdir).filter(f => f.endsWith(".padded"))) {
        for (const line of textLines(readText(join(outdir, f)))) {
            const m = pr.exec(line);
            if (!m) continue;
            const path = normpath(join(ROOT, m[1])).split(ROOT + "/").join("");
            const k = keyOf(path, m[2], m[3], m[5]);
            if (seen.has(k) || !path.startsWith("src/")) continue;
            seen.add(k);
            if (!out.has(m[5])) out.set(m[5], { bytes: 0, holes: 0, tail: 0, file: "", line: 0 });
            const r = out.get(m[5]);
            r.bytes += Number(m[6]);
            if (m[4] === "size of") r.tail += Number(m[6]); else r.holes += 1;
            if (!r.file) { r.file = path; r.line = Number(m[2]); }
        }
    }
    return out;
}

const wsEndNl = (s) => { let b = s.length; while (b > 0 && s.charCodeAt(b - 1) === 10) b--; return b; };
function parseLayoutsRaw(outdir) {
    const recs = new Map();
    for (const f of listDir(outdir).filter(f => f.endsWith(".layout"))) {
        const text = readText(join(outdir, f));
        for (const blk of text.split("*** Dumping AST Record Layout").slice(1)) {
            let h0 = 0;
            while (blk.charCodeAt(h0) === 10) h0++;
            const h1 = blk.indexOf("\n", h0);
            const m = matchAt(/\s*0 \| (struct|union) (.+)$/, blk.slice(h0, h1 < 0 ? wsEndNl(blk) : h1));
            if (!m) continue;
            const name = pyTrim(m[2]);
            if (name.includes("(anonymous") || name.includes("(unnamed") || recs.has(name)) continue;
            const ls = pyStrip(blk, "\n").split("\n");
            const sm = search(/\[sizeof=(\d+)[^\]]*?align=(\d+)/, blk);
            if (!sm) continue;
            const fields = [];
            let bitf = false;
            for (const l of ls.slice(1)) {
                if (l.includes("[sizeof=")) break;
                const fm = matchAt(/\s*(\d+)(?::(\d+)-(\d+))? \|   (\S.*)$/, l);
                if (!fm) continue;
                const decl = pyTrim(fm[4]);
                if (fm[2] !== undefined) bitf = true;
                const nm = matchAt(/(.*?)\s*(\w+)$/, decl);
                if (nm) fields.push([Number(fm[1]), pyTrim(nm[1]), nm[2]]);
            }
            recs.set(name, { size: Number(sm[1]), align: Number(sm[2]), fields, bitfields: bitf, union: m[1] === "union" });
        }
    }
    return recs;
}

const PRIM = { char: 1, "signed char": 1, "unsigned char": 1, _Bool: 1, bool: 1, uint8_t: 1, int8_t: 1, BOOL: 4,
    short: 2, "unsigned short": 2, uint16_t: 2, int16_t: 2, int: 4, "unsigned int": 4, uint32_t: 4,
    int32_t: 4, float: 4, JSAtom: 4, long: 8, "unsigned long": 8, "long long": 8, "unsigned long long": 8,
    uint64_t: 8, int64_t: 8, double: 8, size_t: 8, ssize_t: 8, uintptr_t: 8, intptr_t: 8,
    off_t: 8, time_t: 8, unsigned: 4, JSClassID: 4, pid_t: 4, socklen_t: 4 };

function fieldGeom(ty, recs, aliases) {
    ty = pyTrim(ty.replace(/\b(const|volatile|_Atomic|restrict)\b/g, ""));
    const am = matchAt(/(.*)\[(\d+)\]$/, ty);
    if (am) { const gg = fieldGeom(am[1], recs, aliases); return gg ? [gg[0] * Number(am[2]), gg[1]] : null; }
    if (ty.includes("*") || ty.includes("(")) return [8, 8];
    if (matchAt(/enum\s/, ty)) return [4, 4];
    ty = pyTrim(ty.replace(/^(struct|union|enum)\s+/, ""));
    if (Object.hasOwn(PRIM, ty)) return [PRIM[ty], PRIM[ty]];
    for (const cand of [ty, aliases.get(ty) || ""]) if (recs.has(cand)) return [recs.get(cand).size, recs.get(cand).align];
    return null;
}

function reorder(rec, recs, aliases) {
    if (rec.union || rec.bitfields || !rec.fields.length) return null;
    const geo = [];
    for (const [off, ty, nm] of rec.fields) {
        const gg = fieldGeom(ty, recs, aliases);
        if (gg === null) return null;
        geo.push([gg[0], gg[1], nm, off]);
    }
    const lastFlex = geo.filter(x => x[0] === 0);
    const body = sortedBy(geo.filter(x => x[0] !== 0), x => [-x[1], -x[0]]);
    let off = 0;
    for (const [size, al] of body) off = Math.floor((off + al - 1) / al) * al + size;
    const a = rec.align;
    let best = Math.floor((off + a - 1) / a) * a;
    if (lastFlex.length) best = Math.max(best, off);
    return best;
}

function verdict(r) {
    if (!r) return "no-remark";
    if (r.vec && !r.missed) return "vectorized";
    if (r.vec) return "vectorized-in-some-copies";
    return "not-vectorized";
}

function attachCompiler(loops, licm, gvn) {
    const bf = new Map();
    for (const L of loops) { if (!bf.has(L.file)) bf.set(L.file, []); bf.get(L.file).push(L); }
    for (const [k, v] of bf) bf.set(k, sortedBy(v, L => [L.line, -L.end]));
    for (const L of loops) { L.licm = 0; L.gvn = new Multiset(); }
    const memo = new Map();
    const innermost = (path, line) => {
        const mk = path + "\x00" + line;
        const hit = memo.get(mk);
        if (hit !== undefined) return hit;
        const r = innermostRaw(path, line);
        memo.set(mk, r);
        return r;
    };
    const innermostRaw = (path, line) => {
        let best = null;
        for (const L of bf.get(path) || []) {
            if (L.line > line) break;
            if (L.line <= line && line <= L.end && (best === null || L.end - L.line < best.end - best.line)) best = L;
        }
        return best;
    };
    for (const [path, line, , n] of licm) { const L = innermost(path, line); if (L) L.licm += n; }
    for (const [path, line, by, n] of gvn) { const L = innermost(path, line); if (L) L.gvn.add(by, n); }
}

function pyFixed(x, f) {
    if (!Number.isFinite(x)) return x > 0 ? "inf" : x < 0 ? "-inf" : "nan";
    const neg = x < 0 || Object.is(x, -0);
    const ax = Math.abs(x);
    const buf = new DataView(new ArrayBuffer(8));
    buf.setFloat64(0, ax);
    const hi = buf.getUint32(0), lo = buf.getUint32(4);
    const ex = (hi >>> 20) & 0x7ff;
    let mant = (BigInt(hi & 0xfffff) << 32n) | BigInt(lo);
    let e2;
    if (ex === 0) e2 = -1074; else { mant |= 1n << 52n; e2 = ex - 1075; }
    let num = mant * 10n ** BigInt(f), den = 1n;
    if (e2 >= 0) num <<= BigInt(e2); else den <<= BigInt(-e2);
    let q = num / den;
    const r2 = (num % den) * 2n;
    if (r2 > den || (r2 === den && (q & 1n) === 1n)) q += 1n;
    let digits = q.toString();
    if (f > 0) { digits = digits.padStart(f + 1, "0"); digits = digits.slice(0, -f) + "." + digits.slice(-f); }
    return (neg ? "-" : "") + digits;
}
const pyPct0 = (x) => pyFixed(x * 100, 0) + "%";
function pyFloatRepr(x) {
    if (Number.isInteger(x) && Math.abs(x) < 1e16) return (Object.is(x, -0) ? "-0" : String(x)) + ".0";
    if (!Number.isFinite(x)) return x > 0 ? "inf" : x < 0 ? "-inf" : "nan";
    const [mant, exps] = x.toExponential().split("e");
    const e = Number(exps);
    if (e >= -4 && e < 16) {
        const neg = mant.startsWith("-");
        const ds = mant.replace("-", "").replace(".", "");
        let s;
        if (e < 0) s = "0." + "0".repeat(-e - 1) + ds;
        else if (ds.length > e + 1) s = ds.slice(0, e + 1) + "." + ds.slice(e + 1);
        else s = ds + "0".repeat(e + 1 - ds.length) + ".0";
        return (neg ? "-" : "") + s;
    }
    return mant + "e" + (e < 0 ? "-" : "+") + String(Math.abs(e)).padStart(2, "0");
}
const pyStr = (v) => (v === true ? "True" : v === false ? "False" : v === null || v === undefined ? "None" : v instanceof PyNum ? v.toString() : String(v));
class PyNum {
    constructor(v, isFloat) { this.v = v; this.isFloat = isFloat; }
    toString() { return this.isFloat ? pyFloatRepr(this.v) : String(this.v); }
}
const pyStrRepr = (s) => (s.includes("'") && !s.includes('"') ? '"' + s.replace(/\\/g, "\\\\") + '"' : "'" + s.replace(/\\/g, "\\\\").replace(/'/g, "\\'") + "'");
const pyListRepr = (a) => "[" + a.map(pyStrRepr).join(", ") + "]";
const ljust = (s, w) => String(s).padEnd(w);
const rjust = (s, w) => String(s).padStart(w);

const ZERO = { has: () => false, get: () => undefined };

async function report(outdir, limit = 40, want = null) {
    const files = sourceFiles();
    const defs = defines(files);
    const have = isDir(outdir);
    const [scans, remarkFiles, [padded, recs]] = await scanAndCollect(files, defs, outdir, have,
        () => (have ? [parsePadded(outdir), parseLayouts(outdir)] : [new Map(), new Map()]));
    const [vec, licm, gvn, spills, extra] = applyRemarks(remarkFiles);
    const [pfuncs, plines] = have ? loadProfile(outdir) : [new Map(), new Map()];
    const loops = [], arrays = [], structs = [], franges = [];
    let fnotes = [];
    const extern = new Map(), enums = new Map(), ftypes = new Map();
    for (const [l, a, fn, st, mb] of scans) {
        loops.push(...l); arrays.push(...a); fnotes.push(...fn); structs.push(...st);
        franges.push(...mb.__ranges__);
        for (const [fn_, ids] of mb.__types__) {
            if (!ftypes.has(fn_)) ftypes.set(fn_, new Multiset());
            const t = ftypes.get(fn_);
            for (const [k, n] of ids) t.add(k, n);
        }
        for (const [k, v] of mb.__enums__) enums.set(k, v);
        for (const [k, v] of mb.__extern__) extern.set(k, v);
    }
    attachCompiler(loops, licm, gvn);
    for (const L of loops) {
        const r = vec.get(keyOf(L.file, L.line));
        L.verdict = verdict(r);
        L.reasons = r ? mostCommon(new Map(r.reasons)).slice(0, 3).map(([k, v]) => `${k} x${v}`).join("; ") : "";
        L.vf = r ? [...r.width].sort().join(",") : "";
        const sp = spills.get(keyOf(L.file, L.line));
        [L.spills, L.reloads, L.spillcost] = sp ? [sp[0], sp[1], sp[3]] : [0, 0, 0.0];
    }
    const sbyname = new Map();
    for (const st of structs) {
        if (!sbyname.has(st.name)) sbyname.set(st.name, st);
        if (st.alias && !sbyname.has(st.alias)) sbyname.set(st.alias, st);
    }
    const findings = new Map();
    const push = (check, row) => { if (!findings.has(check)) findings.set(check, []); findings.get(check).push(row); };
    const aliases = new Map(structs.filter(st => st.alias).map(st => [st.alias, st.name]));
    const projectStructs = union(structs.map(st => st.name), aliases.keys());
    const rangeHeat = (fpath, a, b) => {
        const fl = plines.get(fpath);
        if (!fl || !fl.size) return new PyNum(0.0, true);
        const vals = [...fl].filter(([ln]) => a <= ln && ln <= b).map(([, v]) => v);
        return vals.length ? new PyNum(f64Sum(Float64Array.from(vals)), true) : new PyNum(0, false);
    };
    for (const L of loops) { const h = rangeHeat(L.file, L.line, L.end); L.heat = h.v; L.heatStr = h.toString(); }
    const fself = new Map();
    for (const [fpath, a, b, name] of franges) fself.set(name, (fself.get(name) || 0) + rangeHeat(fpath, a, b).v);
    const HOT = 0.005;
    const addF = (check, file, line, func, what, rank, heat = null) => push(check, { file, line, func, what, rank, heat: heat === null ? (fself.get(func) || 0.0) : heat });
    const lrank = (L) => [L.inner ? 0 : 1, L.verdict === "not-vectorized" ? 0 : 1, -L.depth, L.lines];
    const managed = PROJECT.managed_prefixes || [];
    const msSize = (m) => m.size;
    for (const L of loops) {
        const lh = L.heat;
        const add = (check, file, line, func, what, rank, heat = null) => push(check, { file, line, func, what, rank, heat: heat === null ? lh : heat });
        for (const [k, v] of Object.entries(L.gist)) add(k, L.file, L.line, L.func, v.join(",").slice(0, 110) + `  [${L.verdict}]`, lrank(L));
        const heapA = managed.length ? L.alloc.filter(x => !managed.some(p => x.startsWith(p))) : [...L.alloc];
        const heapF = managed.length ? L.free.filter(x => !managed.some(p => x.startsWith(p))) : [...L.free];
        if (heapA.length && heapF.length) add("alloc-churn", L.file, L.line, L.func, `alloc=${heapA.join(",")} free=${heapF.join(",")}`, [L.inner ? 0 : 1, -L.depth]);
        else if (L.alloc.length && L.inner) add("alloc-in-loop", L.file, L.line, L.func, L.alloc.join(",").slice(0, 80), lrank(L));
        if (L.push.length && L.inner) add("push-per-element", L.file, L.line, L.func, L.push.join(",").slice(0, 80), lrank(L));
        if (L.smallcpy.length && L.inner && L.verdict === "not-vectorized") add("smallcpy-blocks-vec", L.file, L.line, L.func, L.smallcpy.join(",") + " :: " + L.reasons.slice(0, 70), lrank(L));
        if (L.libm.length && L.inner && L.verdict === "not-vectorized") add("libm-in-loop", L.file, L.line, L.func, L.libm.join(",") + " :: " + L.reasons.slice(0, 70), lrank(L));
        for (const [k, v] of Object.entries(L.more || {})) add(k, L.file, L.line, L.func, v.slice(0, 160) + `  [${L.verdict}]`, lrank(L));
        for (const cal of L.xcalls || []) {
            const home = extern.get(cal);
            if (home && home !== L.file) add("xtu-call", L.file, L.line, L.func, `${cal}() per element, defined in ${home}  [${L.verdict}]`, lrank(L));
        }
        if (L.fission && L.verdict !== "vectorized" && !L.tail) add("loop-fission", L.file, L.line, L.func, L.fission + `  [${L.verdict}]`, lrank(L));
        if (L.fnptr && L.inner && L.lines <= 12) add("indirect-call-in-loop", L.file, L.line, L.func, `${L.fnptr} indirect :: ${L.header.slice(0, 60)}`, lrank(L));
        for (const [base, flds] of (L._aos || new Map())) {
            const ty = (L._aos_type || new Map()).get(base);
            const rec = recs.get(ty) || recs.get(aliases.get(ty || "") || "");
            if (!rec || rec.size < 32) continue;
            let used = 0;
            for (const [, fty, fnm] of rec.fields) if (flds.includes(fnm)) { const gg = fieldGeom(fty, recs, aliases); used += gg ? gg[0] : 8; }
            if (used && used * 4 <= rec.size) add("aos-partial-touch", L.file, L.line, L.func, `${base}[i].{${flds.join(",")}}: ${used} of ${rec.size} B per element (${ty})`, lrank(L));
        }
        if (L.licm) add("licm-blocked", L.file, L.line, L.func, `${L.licm} load(s) not hoisted  [${L.verdict}]`, [-L.licm]);
        if (msSize(L.gvn)) {
            const ents = [...L.gvn];
            const tot = ents.reduce((s, [, v]) => s + v, 0);
            add("gvn-clobbered", L.file, L.line, L.func, `${tot} reload(s): ` + mostCommon(new Map(ents)).map(([k, v]) => `${k}=${v}`).join(","), [-tot]);
        }
        if (L.vf && L.vf.split(",").every(w => w.startsWith("1x"))) add("interleave-only", L.file, L.line, L.func, `width ${L.vf}`, lrank(L));
        if (L.spills || L.reloads) add("loop-spills", L.file, L.line, L.func, `${L.spills} spills ${L.reloads} reloads, weighted cost ${pyFixed(L.spillcost, 0)}  [${L.verdict}, depth ${L.depth}]`, [-L.spillcost]);
    }
    for (const n of fnotes.filter(x => x.check === "_cold_calls")) {
        if ((fself.get(n.func) || 0.0) >= HOT) addF("cold-path-in-hot-fn", n.file, n.line, n.func, `${n.what} cold call(s) not under unlikely()`, [0]);
    }
    fnotes = fnotes.filter(x => x.check !== "_cold_calls");
    const seenInl = new Set();
    for (const [fpath, ln, caller, callee, cost, thr, why] of extra.inline_costly) {
        const k = keyOf(caller, callee, ln);
        if (why !== "TooCostly" || seenInl.has(k)) continue;
        const site = rangeHeat(fpath, ln, ln).v;
        if (site < 0.001) continue;
        seenInl.add(k);
        addF("inline-too-costly", fpath, ln, caller, `${callee} not inlined (cost ${cost} / threshold ${thr}); call-site line self-time`, [0], site);
    }
    const slpM = new Map(), slpO = new Map();
    for (const [k, n] of extra.slp_missed) { const fn = k.split("\x00")[1]; slpM.set(fn, (slpM.get(fn) || 0) + n); }
    for (const [k, n] of extra.slp_ok) { const fn = k.split("\x00")[1]; slpO.set(fn, (slpO.get(fn) || 0) + n); }
    const fhome = new Map(franges.map(([fpath, a, , name]) => [name, [fpath, a]]));
    for (const [fn, n] of slpM) {
        if ((fself.get(fn) || 0.0) >= HOT) {
            const [fp, ln] = fhome.get(fn) || ["?", 0];
            addF("slp-missed-hot", fp, ln, fn, `${n} SLP attempts missed, ${slpO.get(fn) || 0} vectorized (incl. inlined code)`, [0]);
        }
    }
    const sfile = new Map(structs.map(st => [st.name, st]));
    for (const st of structs) if (st.alias) sfile.set(st.alias, st);
    const hotFuncs = new Map([...fself].filter(([, h]) => h >= 0.01));
    const tcGet = (tc, t, f) => tc.count(t + "\x00" + f);
    for (const [name, rec] of recs) {
        if (!projectStructs.has(name)) continue;
        const st = sfile.get(name);
        const best = reorder(rec, recs, aliases);
        if (best !== null && best < rec.size) {
            const linesNow = Math.floor((rec.size + 63) / 64), linesBest = Math.floor((best + 63) / 64);
            if (rec.size - best >= 8 || linesBest < linesNow)
                addF("reorder-savings", st ? st.file : "?", st ? st.line : 0, name, `${rec.size} -> ${best} B by alignment order (${linesNow} -> ${linesBest} cache lines)`, [best - rec.size], 0.0);
        }
        if (rec.size >= 128 && hotFuncs.size && !rec.union) {
            const weight = new Map();
            const tnames = union([name], st && st.alias ? [st.alias] : [], st ? [st.name] : []);
            const users = new Map();
            for (const [f, h] of hotFuncs) {
                const tc = ftypes.get(f);
                if (!tc || !tc.size) continue;
                for (const [, , fnm] of rec.fields) {
                    let k = 0;
                    for (const t of tnames) k += tcGet(tc, t, fnm);
                    if (k) { weight.set(fnm, (weight.get(fnm) || 0) + h * k); users.set(f, h); }
                }
            }
            const hot = rec.fields.filter(([, , fnm]) => weight.get(fnm));
            if (hot.length >= 2) {
                const touched = new Set(hot.map(([off]) => Math.floor(off / 64)));
                let need = 0;
                for (const [, fty] of hot) { const gg = fieldGeom(fty, recs, aliases); need += gg ? gg[0] : 8; }
                const needLines = Math.max(1, Math.floor((need + 63) / 64));
                if (touched.size >= needLines + 2)
                    addF("hot-fields-spread", st ? st.file : "?", st ? st.line : 0, name,
                        `${hot.length} hot fields (${need} B) span ${touched.size} of ${Math.floor((rec.size + 63) / 64)} lines: ` +
                        sortedBy(hot, x => -weight.get(x[2])).slice(0, 8).map(x => x[2]).join(","),
                        [-(touched.size - needLines)], users.size ? Math.max(...users.values()) : 0.0);
            }
        }
    }
    const typeHeat = new Map();
    for (const L of loops) for (const ty of (L._aos_all_type || new Map()).values()) typeHeat.set(ty, Math.max(typeHeat.get(ty) || 0.0, L.heat));
    const relayout = (rec, repl) => {
        const geo = [];
        for (const [, ty, nm] of rec.fields) {
            const gg = fieldGeom(repl.has(nm) ? repl.get(nm) : ty, recs, aliases);
            if (gg === null || gg[0] === 0) return null;
            geo.push(gg);
        }
        let off = 0;
        for (const [sz, al] of sortedBy(geo, gg => [-gg[1], -gg[0]])) off = Math.floor((off + al - 1) / al) * al + sz;
        const a = geo.length ? Math.max(...geo.map(gg => gg[1])) : 1;
        return Math.floor((off + a - 1) / a) * a;
    };
    const usersHeat = (names) => {
        let best = Math.max(...[...names].map(n => typeHeat.get(n) || 0.0), 0.0);
        if (!names.size) best = 0.0;
        for (const [f, h] of fself) {
            const tc = ftypes.get(f);
            if (tc && tc.size && h > best && tc.elementSet().some(k => names.has(k.split("\x00")[0]))) best = h;
        }
        return best;
    };
    for (const [name, rec] of recs) {
        if (!projectStructs.has(name) || rec.union || rec.bitfields || !rec.fields.length) continue;
        const st = sfile.get(name);
        const tn = union([name], st && st.alias ? [st.alias] : [], st ? [st.name] : []);
        let now = relayout(rec, new Map());
        if (now === null) continue;
        now = Math.min(now, rec.size);
        const links = rec.fields.filter(([, ty]) => { const m = fullmatch(/(?:struct\s+)?(\w+)\s*\*/, pyTrim(ty)); return m && tn.has(m[1]); }).map(x => x[2]);
        if (links.length >= 2 && links.length < rec.fields.length) {
            const nw = relayout(rec, new Map(links.map(nm => [nm, "uint32_t"])));
            if (nw !== null && (now - nw >= Math.max(8, Math.floor(now / 8)) || Math.floor((nw + 63) / 64) < Math.floor((now + 63) / 64)))
                addF("ptr-to-index", st ? st.file : "?", st ? st.line : 0, name, `${links.length} self-links (${links.slice(0, 4).join(",")}): ${now} -> ${nw} B per node with uint32_t pool indices`, [nw - now], usersHeat(tn));
        }
        const narrow = new Map();
        for (const [, ty, nm] of rec.fields) {
            const et = pyTrim(ty).replace(/^enum\s+/, "");
            const rng = enums.get(et);
            const gg = fieldGeom(ty, recs, aliases);
            if (rng && gg && gg[0] === 4 && gg[1] === 4 && rng[0] >= 0) {
                if (rng[1] < 256) narrow.set(nm, "uint8_t");
                else if (rng[1] < 65536) narrow.set(nm, "uint16_t");
            }
        }
        if (narrow.size) {
            const nw = relayout(rec, narrow);
            if (nw !== null && (now - nw >= 8 || Math.floor((nw + 63) / 64) < Math.floor((now + 63) / 64)))
                addF("wide-enum-field", st ? st.file : "?", st ? st.line : 0, name, `${[...narrow].slice(0, 4).map(([k, v]) => `${k}->${v}`).join(",")}: ${now} -> ${nw} B`, [nw - now], usersHeat(tn));
        }
    }
    const bitLength = (n) => (n === 0 ? 0 : Math.floor(Math.log2(n)) + 1);
    for (const [ty, h] of typeHeat) {
        const rec = recs.get(ty) || recs.get(aliases.get(ty) || "");
        if (!rec || rec.union || !projectStructs.has(ty)) continue;
        const frac = dmStraddle(rec.size);
        if (!frac || frac < 0.25) continue;
        const sz = rec.size;
        const up = sz < 64 ? 2 ** bitLength(sz - 1) : Math.floor((sz + 63) / 64) * 64;
        const down = sz < 64 ? 2 ** (bitLength(sz) - 1) : Math.floor(sz / 64) * 64;
        const best = relayout(rec, new Map()) || sz;
        const st = sfile.get(ty) || sfile.get(aliases.get(ty) || "");
        addF("line-straddle", st ? st.file : "?", st ? st.line : 0, ty,
            `${sz} B elements: ${pyPct0(frac)} touch an extra cache line (64-B aligned base); pad to ${up} B or shrink to ${down} B` +
            ` (best field order gives ${best} B)`, [-frac], h);
    }
    const sigs = new Map();
    for (const n of fnotes.filter(x => x.check === "_sig")) {
        sigs.set(n.func, n);
        for (const ty of n.byval) {
            const rec = recs.get(ty) || recs.get(aliases.get(ty) || "");
            if (rec && rec.size >= 32 && projectStructs.has(ty)) addF("ptr-struct-by-value", n.file, n.line, n.func, `parameter of type ${ty} (${rec.size} B) passed by value`, [-rec.size]);
        }
        const rt = n.ret ? words(n.ret).at(-1) : "";
        const rec = recs.get(rt) || recs.get(aliases.get(rt) || "");
        if (rec && rec.size >= 32 && projectStructs.has(rt)) addF("ptr-struct-by-value", n.file, n.line, n.func, `returns ${rt} (${rec.size} B) by value`, [-rec.size]);
    }
    fnotes = fnotes.filter(x => x.check !== "_sig");
    const aliasRe = /unsafe dependent memory|UnsafeDep|runtime pointer checks|cannot identify array bounds/;
    for (const L of loops) {
        if (L._copytypes && L._copytypes.length) {
            for (const ty of L._copytypes) {
                const rec = recs.get(ty) || recs.get(aliases.get(ty) || "");
                if (rec && rec.size >= 32) push("ptr-struct-copy-in-loop", { file: L.file, line: L.line, func: L.func, what: `${ty} (${rec.size} B) copied per iteration`, rank: [-rec.size], heat: L.heat });
            }
        }
        const gstore = L.gvn.count("store");
        const aliasEvidence = aliasRe.test(L.reasons) || L.licm || gstore;
        if (aliasEvidence && sigs.has(L.func)) {
            const sg = sigs.get(L.func);
            const types = new Multiset();
            for (const p of sg.ptrs) types.add(p.split("|")[0]);
            const same = [...types].filter(([, c]) => c >= 2).map(([t]) => t);
            const writes = sg.ptrs.filter(p => p.endsWith("|w"));
            if (same.length && writes.length && !sg.restrict)
                push("ptr-restrict-candidate", { file: L.file, line: L.line, func: L.func, what: `${sg.ptrs.length} pointer params (${same.join(", ")} x2+); aliasing evidence: licm ${L.licm}, store-clobbered reloads ${gstore}; ${L.reasons.slice(0, 50)}`, rank: [0], heat: L.heat });
        }
    }
    for (const a of arrays) addF("stack-array", a.file, a.line, a.func, `${a.bytes}${a.exact ? "" : "?"} B :: ${a.decl}`, [-a.bytes]);
    for (const n of fnotes) addF(n.check, n.file, n.line, n.func, n.what, [n.file, n.line], n.end ? rangeHeat(n.file, n.line, n.end).v : null);
    for (const st of structs) {
        if (st.bools >= 3) addF("bool-fields", st.file, st.line, st.name, `${st.bools} bool fields of ${st.nfields}`, [-st.bools]);
        if ((st.atomics >= 2 || (st.atomics && st.locks)) && !st.aligned)
            addF("false-sharing-review", st.file, st.line, st.name, `${st.atomics} atomics, ${st.locks} locks, ${st.nfields} fields, no 64-B alignment`, [-st.atomics]);
        for (const d of st.dptr) {
            const m = matchAt(/(?:struct\s+)?([A-Za-z_]\w*)\s*\*\s*\*/, d);
            const node = m ? sbyname.get(m[1]) : null;
            if (node && node.selfnext) addF("chained-hash", st.file, st.line, st.name, `${d} -> node ${node.name} (${node.file}:${node.line})`, [st.file]);
        }
    }
    for (const [name, r] of padded) {
        const st = sbyname.get(name);
        addF("padding", r.file, r.line, name, `${r.bytes} B padding (${r.holes} holes, tail ${r.tail})` + (st ? `, ${st.nfields} fields` : ""), [-r.bytes]);
    }
    makedirs(outdir);
    const tsv = join(outdir, "loops.tsv");
    const cols = ["file", "line", "end", "func", "kw", "depth", "inner", "lines", "heat", "verdict", "vf", "spills", "reloads", "spillcost", "licm",
        "alloc", "free", "push", "smallcpy", "libm", "fnptr", "div", "jsapi", "header", "reasons", "gist"];
    const out = [cols.join("\t") + "\n"];
    for (const L of loops) {
        out.push(cols.map(c => {
            const v = L[c];
            if (c === "gist") return Object.entries(v).map(([k, x]) => `${k}=${x.join(",")}`).join(";");
            if (c === "heat") return L.heatStr;
            if (c === "spillcost") return pyFloatRepr(v);
            if (Array.isArray(v)) return v.join(",");
            return pyStr(v);
        }).join("\t") + "\n");
    }
    writeText(tsv, out.join(""));
    const sortRows = (rows) => sortedBy(rows, x => [-x.heat, x.rank]);
    const fo = ["check\tsection\theat_pct\tfile\tline\tfunc\twhat\n"];
    for (const [cid, sec] of CHECKS) for (const x of sortRows(findings.get(cid) || [])) fo.push(`${cid}\t${sec}\t${pyFixed(100 * x.heat, 3)}\t${x.file}\t${x.line}\t${x.func}\t${x.what}\n`);
    writeText(join(outdir, "findings.tsv"), fo.join(""));
    const so = ["file\tline\tfunc\tbytes\texact\tdecl\n"];
    for (const a of sortedBy(arrays, a => -a.bytes)) so.push(`${a.file}\t${a.line}\t${a.func}\t${a.bytes}\t${pyStr(a.exact)}\t${a.decl}\n`);
    writeText(join(outdir, "stack-arrays.tsv"), so.join(""));
    const v = new Multiset();
    for (const L of loops) v.add(L.verdict);
    const reasons = new Multiset();
    for (const L of loops) {
        if (L.verdict === "not-vectorized" && L.inner) for (const part of L.reasons.split("; ")) if (part) { const i = part.lastIndexOf(" x"); reasons.add(i < 0 ? part : part.slice(0, i)); }
    }
    const P = [];
    P.push(`loops: ${loops.length} in ${files.length} files; structs ${structs.length}; remarks for ` +
        `${loops.filter(L => L.verdict !== "no-remark").length} loops; licm ${licm.reduce((a, e) => a + e[3], 0)}, gvn ${gvn.reduce((a, e) => a + e[3], 0)}, ` +
        `loops with spills ${loops.filter(L => L.spills).length}, padded structs ${padded.size}`);
    for (const [k, n] of mostCommon(new Map(v))) P.push(`  ${ljust(k, 28)} ${n}`);
    P.push("why innermost loops were not vectorized (top 12):");
    for (const [k, n] of mostCommon(new Map(reasons)).slice(0, 12)) P.push(`  ${rjust(n, 5)}  ${k}`);
    P.push("\nfindings per check (section of AGENT.md):");
    for (const [cid, sec, desc] of CHECKS) P.push(`  ${rjust((findings.get(cid) || []).length, 6)}  ${ljust(cid, 24)} ${ljust(sec, 16)} ${desc}`);
    if (pfuncs.size) {
        const skip = new Set(["stack-array", "padding", "alloc-heavy-fn", "macro-list-walk", "seq-cst-atomic", "mmap-no-madvise",
            "reorder-savings", "fsync-call", "tcp-nodelay-missing", "sort-call", "big-memset"]);
        let allrows = [];
        for (const [cid, rows] of findings) if (!skip.has(cid)) for (const x of rows) if (x.heat > 0) allrows.push([cid, x]);
        allrows = sortedBy(allrows, r => -r[1].heat);
        P.push(`\n## HOTTEST FINDINGS (share of profile self-time inside the loop / function body / call-site line; ${allrows.length} with heat)`);
        for (const [cid, x] of allrows.slice(0, limit ? Math.max(limit, 80) : 0)) P.push(`  ${rjust(pyFixed(100 * x.heat, 2), 6)}%  ${ljust(cid, 24)} ${x.file}:${x.line} ${x.func} :: ${x.what.slice(0, 100)}`);
    }
    for (const [cid, sec, desc] of CHECKS) {
        if (want && want.size && !want.has(cid)) continue;
        const rows = sortRows(findings.get(cid) || []);
        if (!rows.length) continue;
        P.push(`\n## ${cid} [${sec}] (${rows.length}): ${desc}`);
        for (const x of rows.slice(0, limit)) {
            const hp = pfuncs.size ? `${rjust(pyFixed(100 * x.heat, 2), 6)}%  ` : "";
            P.push(`  ${hp}${x.file}:${x.line} ${x.func} :: ${x.what}`);
        }
    }
    if (fself.size) {
        P.push("\n## TOP FUNCTIONS BY SELF TIME (inlined callees counted at their call lines)");
        for (const [fn, h] of mostCommon(fself).slice(0, 30)) { const [fp, ln] = fhome.get(fn) || ["?", 0]; P.push(`  ${rjust(pyFixed(100 * h, 2), 6)}%  ${fp}:${ln} ${fn}`); }
    }
    P.push(`\nfull tables: ${tsv}, ${join(outdir, "findings.tsv")}, ${join(outdir, "stack-arrays.tsv")}`);
    report.text = P.join("\n");
    if (!report.quiet) say(report.text);
    return findings;
}

const workloads = () => (PROJECT.workloads || []).map(w => [w.name, "args" in w ? w.args : ["@cat", ...w.cat]]);

function childrenSum(nodes, k) {
    const d = nodes[k][0];
    let s = 0, j = k + 1, cd = null;
    while (j < nodes.length && nodes[j][0] > d) { if (cd === null) cd = nodes[j][0]; if (nodes[j][0] === cd) s += nodes[j][1]; j++; }
    return s;
}

function sampleTree(path, image) {
    const lines = readText(path).split("\n");
    const nodeRe = /^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\) \+ \d+  \[(0x[0-9a-f]+)\]/;
    const nodes = [];
    for (const l of lines) {
        if (l.startsWith("Total number in stack") || l.startsWith("Sort by top of stack")) break;
        const m = nodeRe.exec(l);
        if (m) nodes.push([m[1].length, Number(m[2]), pyTrim(m[3]), m[4], parseInt(m[5].slice(2), 16)]);
    }
    let load = null;
    for (const l of lines) {
        const m = matchAt(/\s*(0x[0-9a-f]+) -\s+0x[0-9a-f]+ \+?(\S+)/, l);
        if (m && basename(m[2]) === image && load === null) load = parseInt(m[1].slice(2), 16);
    }
    const leaf = new Multiset(), incl = new Multiset(), stack = [];
    let total = 0;
    nodes.forEach(([d, n, fn, img, addr], k) => {
        while (stack.length && stack.at(-1)[0] >= d) stack.pop();
        if (!stack.length) total += n;
        if (!stack.some(x => x[1] === fn)) incl.add(fn, n);
        stack.push([d, fn]);
        const selfN = n - childrenSum(nodes, k);
        if (selfN > 0 && img === image) leaf.add(String(addr), selfN);
    });
    return [leaf, incl, total, load];
}

function symbolizer() {
    for (const c of ["llvm-symbolizer", "/opt/homebrew/opt/llvm/bin/llvm-symbolizer", "/usr/local/opt/llvm/bin/llvm-symbolizer"]) {
        try { if (run([c, "--version"]).code === 0) return c; } catch (e) { continue; }
    }
    die("dod_scan profile: llvm-symbolizer not found (it separates addresses and expands inlined frames; atos does not)");
}

const die = (msg) => { std.err.puts(msg + "\n"); std.exit(1); };

function textVmaddr(binary) {
    const m = search(/segname __TEXT\s+vmaddr (0x[0-9a-f]+)/, run(["otool", "-l", binary]).stdout);
    return m ? parseInt(m[1].slice(2), 16) : 0x100000000;
}

function symbolize(binary, load, addrs) {
    const base = textVmaddr(binary);
    addrs = [...addrs].sort((a, b) => a - b);
    const inp = addrs.map(a => "0x" + (a - load + base).toString(16) + "\n").join("");
    const r = run([symbolizer(), "--obj=" + binary, "--inlining"], { input: inp });
    const blocks = pyStrip(r.stdout, "\n").split("\n\n");
    const out = new Map();
    if (blocks.length !== addrs.length) die(`dod_scan profile: symbolizer returned ${blocks.length} blocks for ${addrs.length} addresses`);
    addrs.forEach((a, i) => {
        const ls = blocks[i].split("\n");
        const frames = [];
        for (let k = 0; k < ls.length - 1; k += 2) { const m = matchAt(/(.+):(\d+):\d+$/, ls[k + 1]); if (m) frames.push([ls[k], m[1], Number(m[2])]); }
        out.set(a, frames);
    });
    return out;
}

async function profile(outdir, binary, seconds) {
    if (!workloads().length) die('dod_scan profile: no workloads; list them under "workloads" in dod_scan.json');
    binary = abspath(binary);
    const pdir = join(outdir, "profile");
    makedirs(pdir);
    for (const [name, spec] of workloads()) {
        let args;
        if (spec[0] === "@cat") {
            if (!spec.slice(1).every(x => exists(join(ROOT, x)))) { say(`  SKIP ${name}: missing input`); continue; }
            const script = join(pdir, name + ".js");
            writeText(script, spec.slice(1).map(x => readText(join(ROOT, x)) + "\n").join(""));
            args = [script];
        } else {
            if (!exists(join(ROOT, spec[0]))) { say(`  SKIP ${name}: missing ${spec[0]}`); continue; }
            args = spec.map(x => join(ROOT, x));
        }
        const rep = join(pdir, name + ".sample.txt");
        if (exists(rep)) file.remove(new Path(rep));
        const proc = new sys.Spawn(binary, [...(PROJECT.binary_args || []), ...args], { cwd: ROOT, stdin: "ignore", timeoutMs: 600000 });
        const sink = Promise.all([drain(proc.stdout), drain(proc.stderr)]);
        await runAsync(["sample", String(proc.pid), String(seconds), "1", "-mayDie", "-file", rep]);
        await sink;
        await proc.wait();
        proc.close();
        if (!exists(rep)) say(`  SKIP ${name}: no samples (process ended before sampling began)`);
    }
    aggregate(outdir, binary);
}

function aggregate(outdir, binary) {
    binary = abspath(binary);
    const pdir = join(outdir, "profile");
    const funcShare = new Map(), lineShare = new Map();
    let used = 0;
    for (const [name] of workloads()) {
        const rep = join(pdir, name + ".sample.txt");
        if (!exists(rep)) continue;
        const [leaf, incl, total, load] = sampleTree(rep, basename(binary));
        if (!total || load === null) { say(`  SKIP ${name}: empty report`); continue; }
        const syms = symbolize(binary, load, leaf.elementSet().map(Number));
        let own = 0;
        for (const [a, n] of leaf) {
            own += n;
            const seen = new Set();
            for (const [, fl, ln] of syms.get(Number(a)) || []) {
                const key = keyOf(normpath(fl).split(ROOT + "/").join(""), ln);
                if (!seen.has(key)) { seen.add(key); lineShare.set(key, (lineShare.get(key) || 0) + n / total); }
            }
        }
        for (const [fn, n] of incl) funcShare.set(fn, (funcShare.get(fn) || 0) + n / total);
        used++;
        say(`  ${name}: ${total} samples, ${pyFixed(100.0 * own / total, 0)}% in the engine binary`);
    }
    if (!used) die("profile: no workload produced samples");
    writeText(join(outdir, "profile_funcs.tsv"), "func\tinclusive_share\n" + mostCommon(funcShare).map(([fn, v]) => `${fn}\t${pyFixed(v / used, 6)}\n`).join(""));
    writeText(join(outdir, "profile_lines.tsv"), "file\tline\tself_share\n" + mostCommon(lineShare).map(([k, v]) => { const [fl, ln] = k.split("\x00"); return `${fl}\t${ln}\t${pyFixed(v / used, 6)}\n`; }).join(""));
    say(`profile: ${used} workloads -> ${outdir}/profile_funcs.tsv, profile_lines.tsv`);
}

function loadProfile(outdir) {
    const funcs = new Map(), lines = new Map();
    const pf = join(outdir, "profile_funcs.tsv"), pl = join(outdir, "profile_lines.tsv");
    if (exists(pf)) for (const l of textLines(readText(pf)).slice(1)) { const [a, b] = l.split("\t"); funcs.set(a, pyFloat(b)); }
    if (exists(pl)) {
        const byname = new Map();
        for (const f of sourceFiles()) { const rel = relpath(f, ROOT); const b = basename(rel); if (!byname.has(b)) byname.set(b, new Set()); byname.get(b).add(rel); }
        const ambiguous = new Set();
        for (const l of textLines(readText(pl)).slice(1)) {
            let [a, b, c] = l.split("\t");
            const ln = pyInt(b);
            if (ln === 0) continue;
            if (!a.includes("/")) {
                const cands = byname.get(a) || new Set();
                if (cands.size !== 1) { if (cands.size) ambiguous.add(a); continue; }
                a = [...cands][0];
            }
            if (!lines.has(a)) lines.set(a, new Map());
            const m = lines.get(a);
            m.set(ln, (m.get(ln) || 0.0) + pyFloat(c));
        }
        if (ambiguous.size) say(`profile: ${ambiguous.size} basenames map to several sources and were dropped: ${pyListRepr([...ambiguous].sort().slice(0, 6))}`);
    }
    return [funcs, lines];
}
const SELFTEST_SRC = String.raw`
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <math.h>
typedef int BOOL;
struct list_head { struct list_head *next, *prev; };
#define list_for_each(el, head) for (el = (head)->next; el != (head); el = el->next)
void vec_push(int);
int lre_compile(int);
struct S { int n; double* a; long total; };
struct P { char a; long b; char c; int d; };
void g(void);
void lf(struct S* s, const int* items, int m) { for (int i = 0; i < m; i++) { s->a[i] = items[i]; g(); } }
double fp(const double* a, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
int pk(struct P* p) { return p->a + p->c; }
typedef struct { double v[2]; } f64x2;
f64x2 vaddq_f64(f64x2 a, f64x2 b);
f64x2 vld1q_f64(const double* p);
double fx_vsum(const double* x, int n) { f64x2 acc = { { 0, 0 } }; for (int i = 0; i + 2 <= n; i += 2) acc = vaddq_f64(acc, vld1q_f64(&x[i])); return acc.v[0] + acc.v[1]; }
struct node { int key; struct node* next; };
struct table { struct node** buckets; int n; };
struct flags { BOOL a; BOOL b; BOOL c; int x; };
struct shared { _Atomic long hits; _Atomic long misses; pthread_mutex_t mu; };
struct item { int a; int b; double c; long d; double e[2]; };
struct big { int a; char pad1[100]; int b; char pad2[100]; int c; };
struct big2 { int a; char pad1[100]; int b; char pad2[100]; int c; };
void note_f(float);
void fx_fis(float* o, const float* a, const float* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; note_f(x[i]); } }
int fx_big2(struct big2* q) { return q->a; }
struct node2 { int key; struct node2* next; };
int fx_big(struct big* p) { return p->a + p->b + p->c + p->c + p->c; }
int fx_goto(int n) { int i = 0; again: i++; if (i < n) goto again; return i; }
int fx_tmp(int n) { int* t = malloc(sizeof(int) * 64); if (!t) return 0; t[0] = n; n = t[0]; free(t); return n; }
void fx_heavy(void) { void* a = malloc(1); void* b = malloc(2); void* c = calloc(1, 3); void* d = realloc(a, 4); free(b); free(c); free(d); }
void fx_sync(int fd) { fsync(fd); }
int fx_sock(void) { int s = socket(2, SOCK_STREAM, 0); return connect(s, 0, 0); }
struct acc { long total; int n; struct { long sum; } *st; };
void use(void*);
int g_fn(int);
int fx_churn(int n) { for (int i = 0; i < n; i++) { char* p = malloc(16); use(p); free(p); } return 0; }
void fx_bound(struct acc* s, int* a) { for (int i = 0; i < s->n; i++) { a[i] = 0; use(a); } }
void fx_strlen(const char* s, char* o) { for (size_t i = 0; i < strlen(s); i++) o[i] = s[i]; }
void fx_unswitch(int* a, int n, int mode) { for (int i = 0; i < n; i++) { if (mode) a[i] = 1; else a[i] = 2; } }
int fx_cold(int* a, int n) { for (int i = 0; i < n; i++) { if (a[i] < 0) { fprintf(stderr, "bad"); return -1; } } return 0; }
void fx_mod(int* a, int n, int m) { for (int i = 0; i < n; i++) a[i] = i % m + i % 1000 + i % 1024; }
void fx_stride(int* a, const int* b, int n, int k) { for (int i = 0; i < n; i++) a[i] = b[i * k]; }
void fx_accum(struct acc* s, const int* it, int n) { for (int i = 0; i < n; i++) s->total += it[i]; }
void fx_deep(struct acc* s, const int* it, int n) { for (int i = 0; i < n; i++) s->st->sum = it[i]; }
long fx_tr(long* o, const long* a, int n) { long sum = 0; for (int i = 0; i < n; i++) { o[i] = a[i] * 2; sum += o[i]; } return sum; }
void fx_streams(int* a, int* b, int* c, int* d, int* e, int* f, int n) { for (int i = 0; i < n; i++) a[i] = b[i] + c[i] + d[i] + e[i] + f[i]; }
void fx_carried(int* a, int n) { for (int i = 1; i < n; i++) a[i] = a[i - 1] + 1; }
void fx_sys(int fd, const char* b, int n) { for (int i = 0; i < n; i++) write(fd, b + i, 1); }
void fx_lock(pthread_mutex_t* m, _Atomic long* c, int n) { for (int i = 0; i < n; i++) { pthread_mutex_lock(m); atomic_fetch_add(c, 1); } }
void fx_fmt(char* o, const int* a, int n) { for (int i = 0; i < n; i++) o += sprintf(o, "%d,", a[i]); }
int fx_chase(struct node* p) { int s = 0; while (p) { s += p->key; p = p->next; } return s; }
const char* fx_scan(const char* p, const char* e) { while (p < e && *p != '\n') p++; return p; }
int fx_bs(const int* a, int n, int x) { int lo = 0, hi = n; while (lo < hi) { int mid = (lo + hi) >> 1; if (a[mid] < x) lo = mid + 1; else hi = mid; } return lo; }
int cmp(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }
void fx_sort(int* a, int n) { qsort(a, n, sizeof(int), cmp); }
void fx_memset(void) { char big[4096]; memset(big, 0, sizeof big); use(big); }
void fx_realloc(int** p, int n) { int cap = 0; for (int i = 0; i < n; i++) { *p = realloc(*p, (cap + 1) * sizeof(int)); cap++; } }
void fx_writes(int fd) { write(fd, "a", 1); write(fd, "b", 1); }
void* fx_map(int fd) { return mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0); }
long fx_atomic(_Atomic long* c) { return atomic_load(c); }
long fx_aos(const struct item* it, int n) { long s = 0; for (int i = 0; i < n; i++) s += it[i].a; return s; }
void fx_push(int n) { for (int i = 0; i < n; i++) vec_push(i); }
void fx_cpy(char* d, const int* s, int n) { for (int i = 0; i < n; i++) memcpy(d + 4 * i, &s[i], 4); }
double fx_libm(const double* a, int n) { double s = 0; for (int i = 0; i < n; i++) s += sqrt(a[i]) * (double)g_fn(i); return s; }
struct ops { int (*fn)(int); };
void fx_ind(struct ops* o, int* a, int n) { for (int i = 0; i < n; i++) a[i] = o->fn(a[i]); }
void fx_div(int* a, int n, int d) { for (int i = 0; i < n; i++) a[i] = a[i] / d; }
void fx_alloc(char** o, int n) { for (int i = 0; i < n; i++) o[i] = malloc(8); }
void fx_regex(int n) { for (int i = 0; i < n; i++) lre_compile(0); }
int fx_merge(const int* x, int na, const int* y, int nb, int* o) { int p = 0, q = 0, c = 0; while (p < na && q < nb) { if (x[p] < y[q]) p++; else if (y[q] < x[p]) q++; else { o[c++] = x[p]; p++; q++; } } return c; }
void fx_list(struct list_head* h) { struct list_head* el; list_for_each(el, h) { use(el); } }
struct grid { int w; float* cell; struct grid* parent; struct { struct grid* g; } *meta; };
struct rec48 { double a, b, c, d, e, f; };
double fx_rec(struct rec48 r) { return r.a + r.f; }
double fx_copy(const struct rec48* rs, int n) { double s = 0; for (int i = 0; i < n; i++) { struct rec48 r = rs[i]; s += r.a; } return s; }
float fx_sub(float* a, const int* idx, int n) { float t = 0; for (int i = 0; i < n; i++) { a[idx[i] * 2] += 1; t += a[idx[i] * 2]; a[idx[i] * 2] *= t; } return t; }
float fx_chain(struct grid* g, int n) { float t = 0; for (int i = 0; i < n; i++) { t += g->meta->g->cell[i]; g->meta->g->w++; t += g->meta->g->w; } return t; }
void fx_mbase(struct grid* g, int* out, int n) { for (int i = 0; i < n; i++) { out[i] = (int)g->cell[i]; use(out); } }
float fx_row(const float* a, int m, int n) { float t = 0; for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) t += a[i * n + j]; return t; }
float fx_aop(struct grid** items, int n) { float t = 0; for (int i = 0; i < n; i++) t += items[i]->w; return t; }
void fx_unlink(struct node** head, int key) { struct node *p = *head, *prev = NULL; while (p) { if (p->key == key) { if (prev) prev->next = p->next; else *head = p->next; return; } prev = p; p = p->next; } }
void fx_alias(float* d, float* s, int n) { for (int i = 0; i < n; i++) d[i] = s[0] * d[i]; }
size_t fx_slen(const char* s) { return strlen(s) + strlen(s); }
void fx_fuse(float* a, const float* b, float* c, int n) {
    for (int i = 0; i < n; i++) { a[i] = b[i] * 2.0f; }
    for (int i = 0; i < n; i++) { c[i] = b[i] + 1.0f; }
}
float fx_col(float a[][1024], int n) { float t = 0; for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) { t += a[j][i]; } } return t; }
void fx_tile(float b[][1024], float a[][1024], int n) { for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) { b[j][i] = a[i][j]; } } }
struct ev { int kind; float v; };
void ka(float); void kb(float); void kc(float);
void fx_kind(const struct ev* ev, int n) { for (int i = 0; i < n; i++) { switch (ev[i].kind) { case 0: ka(ev[i].v); break; case 1: kb(ev[i].v); break; case 2: kc(ev[i].v); break; } } }
struct ent { int active; float x, vx; };
void fx_flag(struct ent* e, int n) { for (int i = 0; i < n; i++) { if (!e[i].active) continue; e[i].x += e[i].vx; } }
double fx_gather(const int* idx, const double* val, int n) { double t = 0; for (int i = 0; i < n; i++) { t += val[idx[i]]; } return t; }
void ng_xtu(float);
void fx_xtu(const float* a, int n) { for (int i = 0; i < n; i++) { ng_xtu(a[i]); } }
struct tnode { struct tnode* left; struct tnode* right; struct tnode* parent; int key; };
enum shade { SH_A, SH_B, SH_C };
struct cell { enum shade s0; double w; enum shade s1; int k; enum shade s2; };
struct rec40 { double a, b, c, d; int e; int f; };
double fx_straddle(const struct rec40* r, int n) { double t = 0; for (int i = 0; i < n; i++) { t += r[i].a + r[i].f; } return t; }
int fx_tsum(const struct tnode* t, const struct cell* c) { return t->key + c->k + (int)c->s0 + (int)c->s1 + (int)c->s2; }
`;
const SELFTEST_NEG = String.raw`
void ng_unswitch(int* a, int n) { for (int i = 0; i < n; i++) { if (a[i]) a[i] = 1; } }
void ng_mod(int* a, int n) { for (int i = 0; i < n; i++) a[i] = i % 64; }
void ng_stride(int* a, const int* b, int n) { for (int i = 0; i < n; i++) a[i] = b[i]; }
double ng_fp(const double* a, int n) { double s0 = 0, s1 = 0; for (int i = 0; i + 1 < n; i += 2) { s0 += a[i]; s1 += a[i + 1]; } return s0 + s1; }
double ng_vsum(const double* x, int n) { f64x2 a0 = { { 0, 0 } }, a1 = a0; for (int i = 0; i + 4 <= n; i += 4) { a0 = vaddq_f64(a0, vld1q_f64(&x[i])); a1 = vaddq_f64(a1, vld1q_f64(&x[i + 2])); } return a0.v[0] + a1.v[0]; }
double ng_series(int n) { double t = 1, s = 0; for (int i = 1; i < n; i++) { t *= 0.5; s += t; } return s; }
int ng_merge(const int* x, int na, const int* y, int nb) { int p = 0, q = 0, c = 0; while (p < na && q < nb) { int a = x[p], b = y[q]; c += a == b; p += a <= b; q += b <= a; } return c; }
struct ng_ordered { double d; long l; int i; short s; char c; char c2; };
int* ng_tmp(void) { int* t = malloc(64); return t; }
void ng_row(int* o, const int* a, int m, int n) { for (int i = 0; i < m; i++) { const int* row = a + i * n; for (int j = 0; j < n; j++) o[j] = row[j]; } }
void ng_restrict(float* restrict d, const float* restrict s, int n) { for (int i = 0; i < n; i++) d[i] = s[0] * d[i]; }
double ng_byptr(const struct rec48* r) { return r->a; }
size_t ng_slen(const char* s) { size_t n = strlen(s); return n + n; }
void ng_unlink(struct node** head, int key) { struct node** pp = head; while (*pp) { if ((*pp)->key == key) { *pp = (*pp)->next; return; } pp = &(*pp)->next; } }
void ng_accum(int** ps, int n) { for (int i = 0; i < n; i++) { int* p = ps[i]; p[0]++; } }
void ng_elem(struct node* h) { struct node* p; for (p = h; p; p = p->next) p->key++; }
const char* ng_scan(const char* p, const char* e) { return memchr(p, '\n', e - p); }
void ng_sink(float*, int);
void ng_fis(float* o, const float* a, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f; ng_sink(o, i); } }
void ng_xtu(float v) { (void)v; }
void ng_nofuse(float* a, const float* b, float* c, int n) {
    for (int i = 0; i < n; i++) { a[i] = b[i] * 2.0f; }
    for (int i = 0; i < n; i++) { c[i] = a[i + 1]; }
}
int ng_rowwalk(int a[][1024], int n) { int t = 0; for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) { t += a[i][j]; } } return t; }
void ng_twokinds(const struct ev* ev, int n) { for (int i = 0; i < n; i++) { const struct ev* q = &ev[i]; switch (q->kind) { case 0: ka(q->v); break; default: kb(q->v); } } }
void ng_thresh(struct ent* e, int n, float t) { for (int i = 0; i < n; i++) { struct ent* q = &e[i]; if (q->x > t) continue; q->x += q->vx; } }
void ng_lut(int* o, const unsigned char* s, const int* tab, int n) { for (int i = 0; i < n; i++) { o[i] = tab[s[i]]; } }
static void ng_local(float v) { (void)v; }
void ng_samefile(const float* a, int n) { for (int i = 0; i < n; i++) { ng_local(a[i]); } }
`;

const packJSON = (v) => JSON.stringify(v, (k, x) => (x instanceof Multiset ? { __ms: [...x] } : x instanceof Map ? { __m: [...x] } : x instanceof Set ? { __s: [...x] } : x));
const unpackJSON = (t) => JSON.parse(t, (k, x) => {
    if (x && typeof x === "object" && !Array.isArray(x)) {
        if (x.__ms) { const m = new Multiset(); for (const [e, n] of x.__ms) m.add(e, n); return m; }
        if (x.__m) return new Map(x.__m);
        if (x.__s) return new Set(x.__s);
    }
    return x;
});
const SELF = (() => { const a0 = sys.args()[0]; return a0.includes("/") ? abspath(a0) : sys.Which(a0) || a0; })();
const THREADS = Math.max(1, sys.cpuInfo().threads || 1);
const SCAN_COST = 18, INPROC_WEIGHT = 4 << 20;

function shardByWeight(items, weight, n) {
    const bins = Array.from({ length: n }, () => ({ w: 0, items: [] }));
    for (const it of sortedBy(items, x => -weight(x))) { const b = bins.reduce((a, c) => (c.w < a.w ? c : a)); b.items.push(it); b.w += weight(it); }
    return bins.filter(b => b.items.length).map(b => b.items);
}

async function runShards(jobs) {
    const tmp = String(file.makeTempDir("dodscan-shard"));
    try {
        return await pmap(jobs, async (job, i) => {
            if (job.kind === "local") return job.run();
            const inp = join(tmp, `in${i}.json`), outp = join(tmp, `out${i}.json`);
            writeText(inp, packJSON({ root: ROOT, project: PROJECT._file || join(ROOT, "none.json"), ...job }));
            const r = await runAsync([SELF, "--std", SCRIPT, "__shard", inp, outp]);
            if (r.code !== 0) throw new Error(`dod_scan shard ${job.kind} failed (rc ${r.code}): ${r.stderr.slice(-800)}`);
            const res = JSON.parse(readText(outp));
            if (job.kind === "scan") for (const e of res) reviveScan(e[1]);
            return res;
        }, { concurrency: THREADS });
    } finally {
        file.removeAll(new Path(tmp));
    }
}

async function shardMain(inp, outp) {
    const job = unpackJSON(readText(inp));
    configure(job.root, job.project);
    let res;
    if (job.kind === "scan") { res = []; for (const p of job.items) res.push([p, await scanFile(p, job.defs)]); }
    else { writeText(outp, JSON.stringify(job.items.map(it => [it, Array.isArray(it) ? collectRemarks(...it) : foldRemarks(collectRemarks(it))]))); return; }
    writeText(outp, packJSON(res));
}

let CACHE_DIR = null;
let CACHE_VERSION = null;
const cacheVersion = () => (CACHE_VERSION ??= BLAKE3Hex([readText(SCRIPT), JSON.stringify(PROJECT), ROOT].join("\0")));
const FILE_HASH = new Map();
const fileHash = (p) => {
    if (!FILE_HASH.has(p)) {
        let h = null;
        try { h = BLAKE3Hex(file.readFile(P(p), { bytes: true })); } catch (e) { h = null; }
        FILE_HASH.set(p, h);
    }
    return FILE_HASH.get(p);
};
const jsonExact = (v) => { try { return JSON.stringify(JSON.parse(JSON.stringify(v))) === JSON.stringify(v) && !/\bnull\b/.test(JSON.stringify(v)); } catch (e) { return false; } };
function resultCache(name, key, compute) {
    if (!CACHE_DIR) return compute();
    const f = join(CACHE_DIR, name + ".json.zst");
    try {
        const [k, v] = JSON.parse(unzstd(file.readFile(P(f), { bytes: true }), { asString: true }));
        if (k === key) return v;
    } catch (e) { }
    const v = compute();
    if (jsonExact(v)) {
        try { makedirs(CACHE_DIR); file.writeFile(P(f), zstd(JSON.stringify([key, v]), { level: 1 }), { atomic: true }); }
        catch (e) { std.err.puts(`dod_scan: cache not saved: ${e.message || e}\n`); }
    }
    return v;
}
const reviveScan = (v) => {
    for (const L of v[0]) for (const k of ["_aos", "_aos_all", "_aos_all_type", "_aos_type"]) if (L[k]) L[k] = new Map(L[k].__m);
    const mb = v[4];
    mb.__enums__ = new Map(mb.__enums__.__m);
    mb.__extern__ = new Map(mb.__extern__.__m);
    mb.__types__ = new Map(mb.__types__.__m.map(([k, x]) => { const m = new Multiset(); for (const [e, n] of x.__ms) m.add(e, n); return [k, m]; }));
    return v;
};
function cacheLoad(name, plain = false) {
    if (!CACHE_DIR) return new Map();
    try {
        const t = unzstd(file.readFile(P(join(CACHE_DIR, name + ".json.zst")), { bytes: true }), { asString: true });
        return new Map(JSON.parse(t).map(([p, k, v]) => [p, [k, plain ? v : reviveScan(v)]]));
    } catch (e) { return new Map(); }
}
function cacheSave(name, entries, plain = false) {
    if (!CACHE_DIR) return;
    try {
        makedirs(CACHE_DIR);
        file.writeFile(P(join(CACHE_DIR, name + ".json.zst")), zstd(plain ? JSON.stringify(entries) : packJSON(entries), { level: 1 }), { atomic: true });
    } catch (e) { std.err.puts(`dod_scan: cache not saved: ${e.message || e}\n`); }
}

async function scanAndCollect(files, defs, outdir, have, side = () => null) {
    const yamls = have ? listDir(outdir).filter(f => f.endsWith(".opt.yaml")).map(f => join(outdir, f)) : [];
    const ver = cacheVersion(), dkey = BLAKE3Hex(packJSON([...defs]));
    const skeys = new Map(files.map(p => { const h = fileHash(p); return [p, h === null ? null : `${ver}:${dkey}:${h}`]; }));
    const rkeys = new Map(yamls.map(p => { const st = statOf(p); return [p, st ? `${ver}:${st.size}:${st.mtimeMs}` : null]; }));
    const scanned = new Map(), remarks = new Map();
    const reuse = (paths, keys, cache, into) => { for (const p of paths) { const c = cache.get(p); if (c && keys.get(p) !== null && c[0] === keys.get(p)) into.set(p, c[1]); } };
    reuse(files, skeys, cacheLoad("scan"), scanned);
    reuse(yamls, rkeys, have ? cacheLoad("remarks", true) : new Map(), remarks);
    const sMiss = files.filter(p => !scanned.has(p)), rMiss = yamls.filter(p => !remarks.has(p));
    const roundTrip = (v) => unpackJSON(packJSON(v));
    let sideRes;
    const missW = sMiss.reduce((t, p) => t + (statOf(p)?.size ?? 0) * SCAN_COST, 0) + rMiss.reduce((t, p) => t + (statOf(p)?.size ?? 0), 0);
    if (missW <= INPROC_WEIGHT || (rMiss.length === 0 && sMiss.length === 1)) {
        sideRes = side();
        for (const p of sMiss) scanned.set(p, roundTrip(await scanFile(p, defs)));
        for (const p of rMiss) remarks.set(p, JSON.parse(JSON.stringify(foldRemarks(collectRemarks(p)))));
    } else {
        const size = (p) => { const st = statOf(p); return st ? st.size : 0; };
        const n = THREADS, sizes = new Map([...sMiss, ...rMiss].map(p => [p, size(p)]));
        const sW = (p) => sizes.get(p) * SCAN_COST, rW = (it) => (Array.isArray(it) ? sizes.get(it[0]) / it[2] : sizes.get(it));
        const chunkBytes = Math.max(4 << 20, Math.ceil((sMiss.reduce((t, p) => t + sW(p), 0) + rMiss.reduce((t, p) => t + rW(p), 0)) / n));
        const rItems = rMiss.flatMap(p => { const c = Math.ceil(sizes.get(p) / chunkBytes); return c <= 1 ? [p] : Array.from({ length: c }, (_, k) => [p, k, c]); });
        const jobs = sortedBy([...shardByWeight(sMiss, sW, n * 2).map(items => ({ kind: "scan", items, defs, w: items.reduce((t, p) => t + sW(p), 0) })),
            ...shardByWeight(rItems, rW, n * 2).map(items => ({ kind: "remarks", items, w: items.reduce((t, it) => t + rW(it), 0) }))], j => -j.w);
        jobs.splice(Math.min(n - 1, jobs.length), 0, { kind: "local", run: side });
        const parts = new Map();
        for (const [j, res] of (await runShards(jobs)).map((r, i) => [jobs[i], r])) if (j.kind === "local") sideRes = res; else for (const [p, v] of res) {
            if (j.kind === "scan") scanned.set(p, v);
            else if (!Array.isArray(p)) remarks.set(p, v);
            else (parts.get(p[0]) ?? parts.set(p[0], []).get(p[0]))[p[1]] = v;
        }
        for (const [p, vs] of parts) remarks.set(p, foldRemarks(mergeRemarks(vs)));
    }
    if (sMiss.length) cacheSave("scan", files.filter(p => skeys.get(p) !== null).map(p => [p, skeys.get(p), scanned.get(p)]));
    if (rMiss.length) cacheSave("remarks", yamls.filter(p => rkeys.get(p) !== null).map(p => [p, rkeys.get(p), remarks.get(p)]), true);
    return [files.map(p => scanned.get(p)), yamls.map(p => remarks.get(p)), sideRes];
}

const SELFTEST_EDGE = String.raw`
#define FXN 64
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
struct obj { int (*write)(int, const void*, int); int (*close)(int); int fd; };
static int cyc_b(int x);
static int cyc_a(int x) { return x > 0 ? cyc_b(x - 1) : 1; }
static int cyc_b(int x) { return x > 0 ? cyc_a(x - 1) : 0; }
static int self_r(int x) { return x > 0 ? self_r(x - 1) : 0; }
#ifdef ALT
static int twice(int x) { return x * 2; }
#else
static int twice(int x) { printf("%d", x); return x * 2; }
#endif
#ifdef ALT
static int thrice(int x) { printf("%d", x); return x * 3; }
#else
static int thrice(int x) { return x * 3; }
#endif
void note_f(float);
void fis_cyc(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = cyc_a(x[i]); note_f(a[i]); } }
void fis_self(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = self_r(x[i]); note_f(a[i]); } }
void fis_twice(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = twice(x[i]); note_f(a[i]); } }
void fis_thrice(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = thrice(x[i]); note_f(a[i]); } }
void fis_dollar(float* o, const float* a, int* y, const int* x, int n) { int k$1 = 3; for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = x[i] + k$1; note_f(a[i]); } }
void fis_cyc2(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = cyc_a(x[i]); } }
void fis_twice2(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = twice(x[i]); } }
void fis_thrice2(float* o, const float* a, int* y, const int* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = thrice(x[i]); } }
void fis_dollar2(float* o, const float* a, int* y, const int* x, int n) { int k$1 = 3; for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; y[i] = x[i] + k$1; } }
void sys_member(struct obj* o, struct obj s, const char* b, int n) { for (int i = 0; i < n; i++) { o->write(o->fd, b, 1); s.close(s.fd); } }
void sys_plain(int fd, const char* b, int n) { for (int i = 0; i < n; i++) { write(fd, b, 1); } }
void braceless(void** p, int n) { for (int i = 0; i < n; i++) free(p[i]); }
void braceless_w(int fd, char* b, int n) { while (n-- > 0) read(fd, b, 1); }
void outer(int n) { int inner(int k) { return k + 1; }} int after_nested(int* a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
int tmp_inner_free(int n) { int* t = malloc(my_free(t) + 64); t[0] = n; n = t[0]; return n; }
int tmp_inner_free2(int n) { int* t = malloc(64);my_free(t); t[0] = n; return n; }
int tmp_two_arg(int n) { int* t = my_alloc(my_free(t), 64); t[0] = n; n = t[0]; return n; }
void use_buf(char*);
void stk(void) { char buf[FXN]; use_buf(buf); }
struct acc2 { long v; };
void ws_ff(struct acc2* p, int n) { for (int i = 0; i < n; i++) p->v += i; }
void divl(int* o, const int* a, int n, int d) { for (int i = 0; i < n; i++) o[i] = a[i]/d; }
void mmv(char* d, const char* s, int n) { for (int i = 0; i < n; i++) memmove(d + i * 4, s + i * 4, 4); }
void* xrealloc(void*, unsigned long);
void rl(int** p, int n) { int cap = 0; for (int i = 0; i < n; i++) { *p = xrealloc(*p, cap + 16); cap = cap + 16; } }
void stray(void) { }} int after_stray(int* a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i] * a[i]; return s; }
`;
const SELFTEST_EDGE_FINDINGS = [
    "alloc-in-loop|45|rl|xrealloc",
    "indirect-call-in-loop|30|sys_member|1 indirect :: int i = 0; i < n; i++",
    "div-in-loop|42|divl|1  [no-remark]",
    "accum-through-pointer|41|ws_ff|p->v  [no-remark]",
    "loop-fission|22|fis_self|2 pieces: clean {y[]} | blocked by {call:note_f} (independent)  [no-remark]",
    "loop-fission|24|fis_thrice|2 pieces: clean {y[]} | blocked by {call:note_f} (independent)  [no-remark]",
    "loop-fission|26|fis_cyc2|2 pieces: clean {o[]} | blocked by {call:cyc_a} (independent)  [no-remark]",
    "loop-fission|27|fis_twice2|2 pieces: clean {o[]} | blocked by {call:twice} (independent)  [no-remark]",
    "syscall-in-loop|31|sys_plain|write(io)  [no-remark]",
    "syscall-in-loop|33|braceless_w|read(io)  [no-remark]",
    "linear-realloc|45|rl|*p, cap + 16  [no-remark]",
    "local-heap-temp|36|tmp_inner_free2|t = malloc(64) freed locally, ~64 B",
    "local-heap-temp|37|tmp_two_arg|t = my_alloc(64) freed locally, ~64 B",
    "licm-blocked|31|sys_plain|1 load(s) not hoisted  [no-remark]",
    "gvn-clobbered|31|sys_plain|1 reload(s): call=1",
];
const SELFTEST_EDGE_LOOPS = "50d785f394eebf881c25b646b42277e7b0d502822050a402bd5324cbef4f2d9a";
function selftestEdgeYaml(root) {
    const F = join(root, "src", "a.c");
    const doc = (kind, pass, name, line, fn, args) => `--- !${kind}\nPass:            ${pass}\nName:            ${name}\nDebugLoc:        { File: '${F}', Line: ${line}, Column: 3 }\nFunction:        ${fn}\nArgs:\n${args}\n...\n`;
    return doc("Missed", "inline", "TooCostly", 22, "fis_twice", "  - Callee:          twice\n  - String:          ' not inlined because too costly'\n  - Cost:            '500'\n\x0b- Cost: '9999'\n  - Threshold:       '225'")
        + doc("Missed", "inline", "TooCostly", 23, "fis_thrice", "  - Callee:          thrice\n  - Cost:            '700'\n\f- Callee: ghost\n  - Threshold:       '225'")
        + doc("Missed", "licm", "LoadWithLoopInvariantAddressInvalidated", 31, "sys_plain", "  - String:          failed to hoist load")
        + doc("Missed", "gvn", "LoadClobbered", 31, "sys_plain", "  - String:          load of type\n  - ClobberedBy:     call\n\x0b- ClobberedBy: store");
}
async function selftestEdge(d) {
    const out = join(d, "out"), old = ROOT, oldCache = CACHE_DIR;
    makedirs(join(d, "src"));
    makedirs(out);
    writeText(join(d, "src", "a.c"), SELFTEST_EDGE.slice(1));
    writeText(join(out, "src_a.c.opt.yaml"), selftestEdgeYaml(d));
    ROOT = d;
    configure(d, join(d, "none.json"));
    CACHE_DIR = null;
    report.quiet = true;
    try { await report(out, 0, new Set()); } finally { report.quiet = false; ROOT = old; CACHE_DIR = oldCache; configure(old); }
    let bad = 0;
    const got = (readText(join(out, "findings.tsv")) || "").split("\n").slice(1).filter(Boolean)
        .map(l => { const c = l.split("\t"); return [c[0], c[4], c[5], c[6]].join("|"); });
    const want = new Set(SELFTEST_EDGE_FINDINGS), have = new Set(got);
    for (const w of SELFTEST_EDGE_FINDINGS) if (!have.has(w)) { say("  MISS edge " + w); bad++; }
    for (const g of got) if (!want.has(g)) { say("  FALSE edge " + g); bad++; }
    const loops = BLAKE3Hex((readText(join(out, "loops.tsv")) || "") + "\0" + (readText(join(out, "stack-arrays.tsv")) || ""));
    const loopsOk = loops === SELFTEST_EDGE_LOOPS;
    if (!loopsOk) say(`  DIFF edge loops.tsv/stack-arrays.tsv digest ${loops} (want ${SELFTEST_EDGE_LOOPS}); table in ${join(out, "loops.tsv")}`);
    bad += !loopsOk;
    say(`  ${bad ? "FAIL" : "ok  "} edge fixture (lookbehind, purity cycles, last #ifdef definition, braceless bodies, stray braces, \\v/\\f remark args)`);
    return bad;
}

async function selftest() {
    const must = new Set(["alloc-churn", "bound-reload", "call-in-bound", "invariant-branch", "cold-in-loop", "modulo-in-loop",
        "non-unit-stride", "accum-through-pointer", "deep-chain", "transform-and-reduce", "many-streams",
        "loop-carried", "syscall-in-loop", "lock-in-loop", "format-in-loop", "pointer-chase", "bytewise-scan",
        "binary-search", "sort-call", "big-memset", "linear-realloc", "write-sequence", "mmap-no-madvise",
        "seq-cst-atomic", "bool-fields", "false-sharing-review", "chained-hash", "stack-array",
        "aos-partial-touch", "push-per-element", "regex-compile-in-loop", "div-in-loop", "alloc-in-loop",
        "indirect-call-in-loop", "macro-list-walk", "single-vector-accumulator", "branchy-merge",
        "goto-loop", "local-heap-temp", "alloc-heavy-fn", "fsync-call", "tcp-nodelay-missing", "cold-path-in-hot-fn",
        "ptr-repeated-subscript", "ptr-repeated-chain", "ptr-member-base-index", "ptr-row-pointer", "ptr-array-of-pointers",
        "ptr-prev-unlink", "ptr-strlen-repeat", "loop-fission", "loop-fusion", "loop-interchange", "cache-tiling",
        "kind-dispatch", "flag-filter", "random-gather", "xtu-call"]);
    const compilerMust = new Set(["licm-blocked", "gvn-clobbered", "padding", "fp-serial-reduction", "smallcpy-blocks-vec", "libm-in-loop",
        "reorder-savings", "hot-fields-spread", "ptr-struct-by-value", "ptr-struct-copy-in-loop", "ptr-restrict-candidate",
        "ptr-to-index", "wide-enum-field", "line-straddle"]);
    const negAllowed = new Set(["stack-array", "padding", "licm-blocked", "gvn-clobbered", "loop-spills", "alloc-in-loop", "pointer-chase"]);
    const old = ROOT, oldCache = CACHE_DIR;
    let bad = 0, findings;
    const d = String(file.makeTempDir("dodscan"));
    try {
        makedirs(join(d, "src"));
        writeText(join(d, "src", "fx.c"), SELFTEST_SRC);
        writeText(join(d, "src", "ng.c"), SELFTEST_NEG);
        const out = join(d, "out");
        makedirs(out);
        ROOT = d;
        configure(d, join(d, "none.json"));
        try {
            const r = run(["clang", "-O2", "-std=gnu17", "-S", "-o", "/dev/null", "-Wno-everything", "-Wpadded",
                "-Xclang", "-fdump-record-layouts-complete", "-ftrivial-auto-var-init=zero", "-fstack-protector-strong",
                "-fsave-optimization-record=yaml", "-foptimization-record-passes=" + REMARK_PASSES,
                "-foptimization-record-file=" + join(out, "fx.c.opt.yaml"), "src/fx.c"], { cwd: d });
            writeText(join(out, "fx.c.padded"), r.stderr);
            writeText(join(out, "fx.c.layout"), r.stdout);
            const srcLines = SELFTEST_SRC.split("\n");
            const coldLine = srcLines.findIndex(l => l.startsWith("int fx_cold(")) + 1;
            const bigLine = srcLines.findIndex(l => l.startsWith("int fx_big(")) + 1;
            writeText(join(out, "profile_funcs.tsv"), "func\tinclusive_share\nfx_cold\t0.25\nfx_big\t0.2\n");
            writeText(join(out, "profile_lines.tsv"), `file\tline\tself_share\nsrc/fx.c\t${coldLine}\t0.125\nsrc/fx.c\t${bigLine}\t0.05\n`);
            if (r.code) { say("selftest: the fixture does not compile:\n" + r.stderr.slice(-800)); return 1; }
            report.quiet = true;
            const tables = () => ["loops", "findings", "stack-arrays"].map(t => readText(join(out, t + ".tsv"))).join("\0") + "\0" + report.text;
            try {
                CACHE_DIR = join(d, "cache");
                findings = await report(out, 40, null);
                const cold = tables();
                await report(out, 40, null);
                const warmOk = tables() === cold;
                say(`  ${warmOk ? "ok  " : "DIFF"} warm-cache run reproduces the cold run's tables`);
                bad += !warmOk;
            } finally { report.quiet = false; CACHE_DIR = oldCache; }
        } finally {
            ROOT = old;
            configure(old);
        }
        bad += await selftestEdge(join(d, "edge"));
    } finally {
        file.removeAll(new Path(d));
    }
    const pos = new Map(), neg = new Map();
    const heatOk = (findings.get("cold-in-loop") || []).some(x => x.func === "fx_cold" && Math.abs(x.heat - 0.125) < 1e-9);
    say(`  ${heatOk ? "ok  " : "MISS"} heat join (loop self-time from profile_lines.tsv)`);
    bad += !heatOk;
    const typedOk = !(findings.get("hot-fields-spread") || []).some(x => x.func === "big2");
    say(`  ${typedOk ? "ok  " : "FALSE"} hot fields are bound to the accessing variable's type (big2 shares names, has no hot user)`);
    bad += !typedOk;
    for (const [cid, rows] of findings) {
        for (const x of rows) {
            if (x.file.endsWith("ng.c")) { if (!neg.has(cid)) neg.set(cid, []); neg.get(cid).push(`${x.func}:${x.what}`); }
            else { if (!pos.has(cid)) pos.set(cid, new Set()); pos.get(cid).add(x.func); }
        }
    }
    for (const cid of [...union(must, compilerMust)].sort()) {
        const hit = !!(pos.get(cid) && pos.get(cid).size);
        say(`  ${hit ? "ok  " : "MISS"} ${ljust(cid, 24)} ${[...(pos.get(cid) || [])].sort().join(",").slice(0, 80)}`);
        bad += !hit;
    }
    for (const [cid, rows] of sortedBy([...neg], x => x[0])) {
        if (negAllowed.has(cid) || cid === "alloc-heavy-fn") continue;
        say(`  FALSE ${ljust(cid, 24)} ${pyListRepr(rows)}`);
        bad += 1;
    }
    const unknown = [...findings.keys()].filter(k => !CHECK_IDS.includes(k));
    if (unknown.length) { say("  checks emitted but not declared: " + pyListRepr(unknown.sort())); bad += 1; }
    say(`selftest: ${!bad ? "ok" : bad + " failure(s)"}`);
    return bad;
}

async function main() {
    if (scriptArgs[1] === "__shard") { await shardMain(scriptArgs[2], scriptArgs[3]); return; }
    const CMDS = ["remarks", "profile", "aggregate", "report", "all", "selftest"];
    const cli = new Command("dod_scan")
        .describe("data-oriented-design scan of a C code base, joined with compiler remarks and an optional profile")
        .argument("cmd", CMDS.join("|"))
        .option("--root <dir>", "project root (default: nearest dir with dod_scan.json, compile_commands.json or .git)", { type: "string" })
        .option("--project <file>", "project config JSON (default: <root>/dod_scan.json or <root>/tools/dod_scan.json)", { type: "string" })
        .option("--binary <path>", "binary the profile workloads run", { type: "string" })
        .option("--seconds <n>", "seconds to sample each workload", { type: "number", default: 20 })
        .option("--out <dir>", "output directory", { type: "string" })
        .option("--jobs <n>", "translation units compiled at a time", { type: "number", default: 2 })
        .option("--limit <n>", "rows per check table", { type: "number", default: 40 })
        .option("--check <name>", "print only these checks' tables (repeatable)", { type: "string", variadic: true })
        .option("--cache <dir>", "per-file result cache (default: project \"cache\", else a temp dir per root)", { type: "string" })
        .option("--no-cache", "scan everything afresh and leave the cache alone", { type: "boolean" });
    let parsed;
    try { parsed = cli.parse(scriptArgs.slice(1)); } catch (e) { std.err.puts(cli.help() + "\n" + (e.message || e) + "\n"); std.exit(2); }
    const a = parsed.options;
    const cmd = parsed.arguments[0];
    if (!CMDS.includes(cmd)) { std.err.puts(cli.help() + `\ndod_scan: argument cmd: invalid choice: '${cmd}' (choose from ${CMDS.map(c => `'${c}'`).join(", ")})\n`); std.exit(2); }
    const checks = a.check === undefined ? null : [].concat(a.check);
    configure(a.root || null, a.project || null);
    const outdir = a.out ? abspath(a.out) : DEFAULT_OUT;
    CACHE_DIR = a["no-cache"] ? null : a.cache ? abspath(a.cache) : PROJECT.cache ? join(ROOT, PROJECT.cache)
        : join(String(Path.temp()), "dod_scan-" + BLAKE3Hex(ROOT).slice(0, 12));
    const binary = a.binary || join(ROOT, PROJECT.binary || "a.out");
    if (cmd === "selftest") std.exit((await selftest()) ? 1 : 0);
    makedirs(outdir);
    if (cmd === "remarks" || cmd === "all") {
        const cmds = remarkCmds();
        say(`remarks: ${cmds.length} translation units, ${a.jobs} at a time, nice 19, passes ${REMARK_PASSES}`);
        let bad = 0;
        for (const [src, rc, err] of await pmap(cmds, (j) => runRemark(j, outdir), { concurrency: a.jobs })) {
            if (rc) { bad++; say(`  FAIL ${src}: ${pyTrim(err).slice(-300)}`); }
        }
        say(`remarks: done, ${bad} failed`);
        if (bad) std.exit(1);
    }
    if (cmd === "profile" || cmd === "all") await profile(outdir, binary, a.seconds);
    if (cmd === "aggregate") aggregate(outdir, binary);
    if (cmd === "report" || cmd === "all") await report(outdir, a.limit, checks ? new Set(checks) : null);
}

await main();
