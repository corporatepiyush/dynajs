import * as file from "dyna:file";
import * as sys from "dyna:sys";
import * as time from "dyna:time";

const T0 = Number(time.monotonicNano());
const nowMs = () => (Number(time.monotonicNano()) - T0) / 1e6;
const phases = [];
let phaseStart = 0;
const phase = (name) => { const t = nowMs(); phases.push([name, Math.round(t - phaseStart)]); phaseStart = t; };

const USAGE = `usage: dynajs tools/sweep.js [--selftest] [--only CAT,...|RULE,...] [--files a,b|@list] [--out DIR] [--root DIR] [--samples N] [--update-baseline] [--patch FILE [--since YYYY-MM-DD]]
  one pass over src tests tools examples bench + root files; rules are data (RULES, JOINS) with pos/neg fixtures checked by --selftest.
  writes OUT/{SUMMARY.txt,findings.tsv,delta.tsv,by-rule/<id>.tsv,status.txt,sweep.json,rule18.tsv[,samples.txt]}; OUT defaults to <agent-work>/sweep.
  --patch FILE: R6 merge guard, lists lines FILE removes that a merged patch added and main still has; R7: git apply --check on --root, and when it
  fails, names a merged patch whose reversal makes FILE apply (cut cumulatively) or says it does not apply; exit 1 on any. Merged = a *.patch under .agent-work
  (mtime >= --since, default 3 days) named by a merges.tsv row dated >= --since: a field path ending in .patch, or a patch-column token equal to the stem.
  baseline.tsv rows may carry a 5th column "justified: <reason>" that --update-baseline keeps.
  delta and the previous-run snapshot only move on a full run (no --only/--files); baseline.tsv is shrink-only (--update-baseline refuses growth).`;

function scriptArgs(av) {
    let i = av.findIndex((a, k) => k > 0 && /(?:^|\/)sweep\.js$/.test(a));
    if (i < 0) i = av.findIndex((a, k) => k > 0 && !a.startsWith("-"));
    return i < 0 ? [] : av.slice(i + 1);
}
function parseArgs(av) {
    const o = { selftest: false, only: null, files: null, out: null, root: null, samples: 0, updateBaseline: false };
    for (let i = 0; i < av.length; i++) {
        const a = av[i];
        const val = () => { if (i + 1 >= av.length) throw new Error(a + " needs a value"); return av[++i]; };
        if (a === "--selftest") o.selftest = true;
        else if (a === "--only") o.only = new Set(val().split(",").map((s) => s.trim()).filter(Boolean));
        else if (a === "--files") o.files = val();
        else if (a === "--out") o.out = val();
        else if (a === "--root") o.root = val();
        else if (a === "--samples") o.samples = Math.max(0, parseInt(val(), 10) || 0);
        else if (a === "--update-baseline") o.updateBaseline = true;
        else if (a === "--patch") o.patch = val();
        else if (a === "--since") o.since = val();
        else if (a === "-h" || a === "--help") { console.log(USAGE); sys.exit ? sys.exit(0) : null; o.help = true; }
        else throw new Error("unknown argument " + a + "\n" + USAGE);
    }
    return o;
}

const esc = (s) => s.replace(/[.*+?^${}()|[\]\\\/]/g, "\\$&");
const SEVRANK = { high: 0, med: 1, low: 2, info: 3 };

function lineStarts(s) {
    const a = [0];
    for (let i = s.indexOf("\n"); i >= 0; i = s.indexOf("\n", i + 1)) a.push(i + 1);
    return a;
}
function lineAt(ls, idx) {
    let lo = 0, hi = ls.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (ls[m] <= idx) lo = m; else hi = m - 1; }
    return lo;
}
function lineText(s, ls, ln) {
    if (ln < 0 || ln >= ls.length) return "";
    const end = ln + 1 < ls.length ? ls[ln + 1] - 1 : s.length;
    return s.slice(ls[ln], end);
}
function linesRange(s, ls, a, b) {
    a = Math.max(0, a); b = Math.min(ls.length - 1, b);
    if (b < a) return "";
    const end = b + 1 < ls.length ? ls[b + 1] - 1 : s.length;
    return s.slice(ls[a], end);
}

function blank(t) { return t.replace(/[^\n]/g, " "); }
function maskC(s) {
    const re = /\/\/|\/\*|"|'/g;
    const out = [];
    let last = 0, m;
    while ((m = re.exec(s))) {
        const i = m.index;
        if (m[0] === "//") {
            let e = s.indexOf("\n", i); if (e < 0) e = s.length;
            out.push(s.slice(last, i), blank(s.slice(i, e))); last = e; re.lastIndex = e;
        } else if (m[0] === "/*") {
            let e = s.indexOf("*/", i + 2); e = e < 0 ? s.length : e + 2;
            out.push(s.slice(last, i), blank(s.slice(i, e))); last = e; re.lastIndex = e;
        } else {
            const q = m[0];
            let j = i + 1;
            while (j < s.length && s[j] !== q && s[j] !== "\n") j += s[j] === "\\" ? 2 : 1;
            if (j > s.length) j = s.length;
            out.push(s.slice(last, i + 1), blank(s.slice(i + 1, j))); last = j; re.lastIndex = j + 1;
        }
    }
    out.push(s.slice(last));
    return out.join("");
}
function stripTsComments(s) {
    return maskC(s);
}

function splitFns(code) {
    const fns = [];
    const re = /[{};]/g;
    let depth = 0, lastTop = 0, cur = null, m;
    while ((m = re.exec(code))) {
        const c = m[0];
        if (c === "{") {
            if (depth === 0) {
                const hdr = code.slice(lastTop, m.index);
                const mm = /([A-Za-z_]\w*)\s*\(((?:[^()]|\([^()]*\))*)\)\s*(?:__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)\s*)*$/.exec(hdr);
                if (mm && !/=\s*$/.test(hdr) && !/^(?:if|for|while|switch|return|sizeof)$/.test(mm[1]))
                    cur = { name: mm[1], hdr: hdr.trim(), params: mm[2], open: m.index, close: -1 };
                else cur = null;
            }
            depth++;
        } else if (c === "}") {
            depth--;
            if (depth <= 0) {
                depth = 0;
                if (cur) { cur.close = m.index; fns.push(cur); cur = null; }
                lastTop = m.index + 1;
            }
        } else if (depth === 0) lastTop = m.index + 1;
    }
    return fns;
}
function fnAt(fns, idx) {
    let lo = 0, hi = fns.length - 1, best = null;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (fns[mid].open <= idx) { best = fns[mid]; lo = mid + 1; } else hi = mid - 1;
    }
    return best && idx <= best.close ? best : null;
}

class Ctx {
    constructor(path, raw) { this.path = path; this.raw = raw; this._code = null; this._ls = null; this._fns = null; }
    get isC() { return /\.[ch]$/.test(this.path); }
    get code() { if (this._code === null) this._code = this.isC ? maskC(this.raw) : this.raw; return this._code; }
    get ls() { if (this._ls === null) this._ls = lineStarts(this.raw); return this._ls; }
    get fns() { if (this._fns === null) this._fns = this.isC ? splitFns(this.code) : []; return this._fns; }
    line(idx) { return lineAt(this.ls, idx); }
    text(ln) { return lineText(this.raw, this.ls, ln); }
}

const CSRC = /^src\/.*\.[ch]$/;
const DTS = /^src\/(?:modules\/[^/]+|types)\/[^/]+\.d\.ts$/;
const SH = /\.(?:sh|bash)$/;
const TESTJS = /^tests\/.*\.(?:js|mjs)$/;
const TOOLJS = /^tools\/.*\.(?:js|mjs)$/;
const LINUXONLY = /(?:^|\/)(?:docker|android)\/|^tests\/ctest\/linux|Dockerfile/;
const SELF = /^tools\/sweep\.js$/;

const RULES = [];
const rule = (id, cat, sev, name, glob, re, o = {}) => RULES.push({ id, cat, sev, name, glob, re, on: "code", ...o });

rule("A01", "A", "low", "alloca", CSRC, /(?<![\w.>])alloca\s*\(/g,
    { key: "alloca", when: (m, f) => !/^\s*#\s*define\b/.test(f.text(f.line(m.index))), hint: "stack allocation sized at run time: bound it against the stack limit or heap-allocate",
      pos: ["void g(size_t k){ void* p = alloca(k); }"], neg: ["#define my_alloca(n) alloca(n)", "void g(void){ p = my_alloca_free(); }"] });
rule("A02", "A", "med", "fmt-size_t", CSRC,
    /\b(?:s?n?printf|fprintf|dprintf|JS_Throw\w*)\s*\([^;]*?"[^"]*%(?:l?d|i)[^"]*"[^;]*?,\s*(?:strlen|sizeof)\s*\(?[\w.>\-\s]*\)?\s*\)\s*;/g,
    { on: "raw", key: "%", hint: "size_t argument printed with %d/%ld: use %zu",
      pos: ['    printf("n=%d\\n", strlen(s));', '    JS_ThrowRangeError(ctx, "bad %ld", sizeof(x));'],
      neg: ['    printf("n=%zu\\n", strlen(s));', '    printf("n=%d\\n", (int)strlen(s));'] });
rule("A03", "A", "med", "narrowing-len", CSRC,
    /\b(?:int|unsigned(?:\s+int)?|int32_t|uint32_t|short|uint16_t)\s+\w+\s*=\s*(?:strlen|strnlen|read|write|recv|send|recvfrom|sendto|pread|pwrite|ftell|lseek|ftello)\s*\(/g,
    { hint: "size_t/ssize_t/off_t result narrowed into a 32-bit int",
      pos: ["    int n = strlen(s);", "    unsigned k = read(fd, b, sz);"],
      neg: ["    size_t n = strlen(s);", "    int n = (int)strlen(s);", "    ssize_t r = read(fd, b, sz);"] });
rule("A07", "A", "low", "if-0", CSRC, /^[ \t]*#[ \t]*if[ \t]+0\b/gm,
    { key: "#", hint: "dead code block: delete it", pos: ["#if 0\nint x;\n#endif"], neg: ["#if 0x1\n#endif", "#ifdef FOO\n#endif"] });
rule("A09", "A", "high", "assert-side-effect", CSRC, /\bassert\s*\(((?:[^;()]|\((?:[^;()]|\([^;()]*\))*\))*)\)\s*;/g,
    { key: "assert", when: (m) => /(?<![=!<>+\-*\/%&|^])=(?!=)|\+\+|--(?!>)/.test(m[1]),
      hint: "assert() body has a side effect that NDEBUG compiles out",
      pos: ["    assert(n++ < 4);", "    assert((p = get()) != NULL);"],
      neg: ["    assert(n <= 4 && p != NULL);", "    assert(a == b);", "    assert(x->y >= 0);"] });
rule("A10", "A", "high", "cmp-literal-length", CSRC,
    /\b(strn(?:case)?cmp|memcmp)\s*\(\s*(?:[^,;"]+?,\s*"((?:[^"\\\n]|\\.)*)"|"((?:[^"\\\n]|\\.)*)"\s*,\s*[^,;"]+?)\s*,\s*(\d+)\s*\)/g,
    { on: "raw", key: "cmp", when: (m) => { const L = cLitLen(m[2] !== undefined ? m[2] : m[3]); const n = +m[4]; return m[1] === "memcmp" ? n < L || n > L + 1 : n < L; },
      hint: "length argument does not match the string literal (prefix compare too short, or memcmp reads past the literal)",
      pos: ['    if (!strncmp(s, "content-type", 7)) x();', '    if (!memcmp(p, "GET ", 6)) x();', '    if (!strncasecmp("abc", s, 2)) x();'],
      neg: ['    if (!strncmp(s, "content-type", 12)) x();', '    if (!memcmp(p, "GET ", 4)) x();', '    if (!memcmp(p, "SQLite format 3", 16)) x();', '    if (!strncmp(s, "a\\nb", 3)) x();'] });
rule("A11", "A", "med", "open-no-cloexec", CSRC, /(?<![\w.>])open(?:at)?\s*\(((?:[^;()]|\([^;()]*\))*)\)/g,
    { key: "open", when: (m) => /\bO_[A-Z]/.test(m[1]) && !/O_CLOEXEC/.test(m[1]), unless: { scope: "fn", re: /cloexec/i },
      hint: "fd opened without O_CLOEXEC leaks into spawned children",
      pos: ["int f(const char* p){\n    int fd = open(p, O_RDONLY);\n    return fd;\n}"],
      neg: ["int f(const char* p){\n    int fd = open(p, O_RDONLY | O_CLOEXEC);\n    return fd;\n}", "int f(void){\n    int fd = open(p, O_RDONLY);\n    fcntl(fd, F_SETFD, FD_CLOEXEC);\n    return fd;\n}"] });
rule("A12", "A", "med", "socket-no-cloexec", CSRC, /(?<![\w.>])(?:socket|accept|pipe|socketpair)\s*\(/g,
    { key: ["socket", "accept", "pipe"], except: /^src\/fuzz\//, unless: { scope: "fn", re: /cloexec|SOCK_CLOEXEC/i },
      hint: "socket/accept/pipe fd without CLOEXEC in the same function leaks into spawned children",
      pos: ["int f(void){\n    int s = socket(AF_INET, SOCK_STREAM, 0);\n    return s;\n}"],
      neg: ["int f(void){\n    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);\n    return s;\n}", "int f(void){\n    int p[2];\n    pipe(p);\n    set_cloexec(p[0]);\n    return 0;\n}"] });
rule("A13", "A", "med", "nonreentrant-libc", CSRC, /(?<![\w.>])(?:localtime|gmtime|ctime|asctime|getpwnam|getpwuid|getgrnam|getgrgid|gethostbyname|gethostbyaddr|inet_ntoa|ttyname|getlogin)\s*\(/g,
    { when: (m, f) => !inWin32(f.code, m.index), hint: "static-buffer libc call is not thread-safe with pool workers: use the _r variant",
      pos: ["    struct tm* t = gmtime(&now);", "    struct tm* t = localtime(&now);", "#ifndef _WIN32\n    x();\n#else\n    y();\n#endif\n    tm = gmtime(&t);"],
      neg: ["#if defined(_WIN32)\n    tm = gmtime(&t);\n#else\n    gmtime_r(&t, &b);\n#endif", "#ifndef _WIN32\n    gmtime_r(&t, &b);\n#else\n    tm = gmtime(&t);\n#endif", "    strerror_r(errno, b, sizeof b);", "    localtime_r(&now, &tm);", "    my_strerror(x);"] });
rule("A14", "A", "med", "weak-rng", CSRC, /(?<![\w.>])(?:rand|srand|random|srandom|drand48|lrand48)\s*\(/g,
    { key: "rand", hint: "libc PRNG: predictable and not thread-safe; use the CSPRNG helper",
      pos: ["    int r = rand() % 10;", "    srand(time(NULL));"], neg: ["    js_random_bytes(b, 8);", "    uint32_t r = xrand(s);"] });
rule("A15", "A", "high", "discarded-jsvalue", CSRC,
    /^[ \t]*(?:\(void\)\s*)?JS_(?:Call|CallConstructor|Invoke|GetProperty|GetPropertyStr|GetPropertyUint32|GetPropertyInt64|GetPropertyValue|Eval|EvalFunction|EvalThis|NewObject\w*|NewArray|NewString\w*|NewError|NewCFunction\w*|ToString|ToObject|GetGlobalObject|ReadObject|PromiseResult|GetException)\s*\(/gm,
    { key: "JS_", when: (m, f) => stmtStart(f, m.index), hint: "returned JSValue dropped: leaks the value and hides an exception",
      pos: ["    JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);", "    (void)JS_GetException(ctx);"], negDup: 0,
      neg: ["    JS_FreeValue(ctx, JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL));", "    v = JS_GetException(ctx);", "    return JS_NewObject(ctx);", "    JS_DupValue(ctx, argv[0]);", "    f(ctx,\n        JS_ToString(ctx, v),\n        x);"] });
rule("A16", "A", "high", "alloc-free-mismatch", CSRC, null,
    { fn: fnAllocMismatch, key: "free", hint: "pointer from js_* allocator freed with libc free (or the reverse)",
      pos: ["void f(JSContext* ctx){\n    char* p = js_malloc(ctx, 8);\n    free(p);\n}", "void f(JSContext* ctx){\n    char* p = strdup(s);\n    js_free(ctx, p);\n}"],
      neg: ["void f(JSContext* ctx){\n    char* p = js_malloc(ctx, 8);\n    js_free(ctx, p);\n}", "void f(void){\n    char* p = malloc(8);\n    free(p);\n}"] });
rule("A19", "A", "high", "unsigned-negative-check", CSRC, null,
    { fn: fnUnsignedNeg, key: ["< 0", ">= 0", "0 >"], hint: "unsigned value compared < 0 / >= 0: always false/true",
      pos: ["int f(size_t n){\n    if (n < 0)\n        return -1;\n    return 0;\n}", "int f(void){\n    uint32_t k = g();\n    if (k >= 0) return 1;\n    return 0;\n}"],
      neg: ["int f(ssize_t n){\n    if (n < 0)\n        return -1;\n    return 0;\n}", "int f(size_t* p){\n    if (p < 0) return 1;\n    return 0;\n}", "int f(unsigned c){\n    if ((int)c < 0) return 1;\n    return 0;\n}"] });
rule("A20", "A", "high", "fork-exit", CSRC, null,
    { fn: fnForkExit, key: "fork", hint: "exit() in a forked child runs atexit handlers and flushes parent stdio: use _exit",
      pos: ["void f(void){\n    pid_t p = fork();\n    if (p == 0) {\n        execvp(a[0], a);\n        exit(127);\n    }\n}"],
      neg: ["void f(void){\n    pid_t p = fork();\n    if (p == 0) {\n        execvp(a[0], a);\n        _exit(127);\n    }\n}"] });

rule("A21", "A", "high", "exception-restore-drops-uncatchable", CSRC, null,
    { fn: fnExcRestore, key: "current_exception", hint: "saved rt->current_exception written back without saving/restoring current_exception_is_uncatchable: an interrupt becomes catchable",
      pos: ["void f(JSContext* ctx){\n    JSValue ex = ctx->rt->current_exception;\n    ctx->rt->current_exception = JS_UNINITIALIZED;\n    g(ctx);\n    ctx->rt->current_exception = ex;\n}"],
      neg: ["void f(JSContext* ctx){\n    JSValue ex = ctx->rt->current_exception;\n    BOOL u = ctx->rt->current_exception_is_uncatchable;\n    ctx->rt->current_exception = JS_UNINITIALIZED;\n    g(ctx);\n    ctx->rt->current_exception = ex;\n    ctx->rt->current_exception_is_uncatchable = u;\n}", "void t(JSRuntime* rt, JSValue obj){\n    rt->current_exception = obj;\n}"] });
rule("A22", "A", "high", "cfunc-call-outside-boundary", CSRC, /(\w+)->u\.cfunc\.c_function\.(\w+)\s*\(/g,
    { key: "c_function.", when: (m, f) => { const fn = fnAt(f.fns, m.index); return m[1] !== "mf_cf" && m[2] !== "f_f" && m[2] !== "f_f_f" && !(fn && CFUNC_BOUNDARY.has(fn.name)); },
      hint: "native function called through u.cfunc.c_function outside the six patched boundaries (js_call_c_function, js_call_cfunc_generic, js_c_function_data_call, js_iterator_step, the two mf_cf method fast paths); f_f and f_f_f take and return doubles, cannot run JS or take an exception, and are exempt",
      pos: ["static JSValue h(JSContext* ctx, JSObject* p){\n    return p->u.cfunc.c_function.generic(ctx, JS_UNDEFINED, 0, NULL);\n}"],
      neg: ["static JSValue h(JSContext* ctx, JSObject* p, double a){\n    return JS_NewFloat64(ctx, p->u.cfunc.c_function.f_f(a));\n}", "static JSValue js_call_c_function(JSContext* ctx, JSObject* p){\n    return p->u.cfunc.c_function.generic(ctx, JS_UNDEFINED, 0, NULL);\n}", "static JSValue h(JSContext* ctx, JSObject* mf_cf){\n    return mf_cf->u.cfunc.c_function.generic(ctx, JS_UNDEFINED, 0, NULL);\n}"] });

rule("C07", "C", "high", "dts-doc-unterminated", DTS, /\/\*\*/g,
    { on: "raw", key: "/**", when: (m, f) => { const c = f.raw.indexOf("*/", m.index + 3); const o = f.raw.indexOf("/**", m.index + 3); return c < 0 || (o >= 0 && o < c); },
      hint: "doc comment not closed before the next /**: the declarations between are commented out, and tsc and dts-bundle both accept it",
      pos: ["/** a. *\nfunction f(): void;\n/** b */\nfunction g(): void;", "/** never closed\nfunction f(): void;"],
      neg: ["/** a. */\nfunction f(): void;\n/** b */\nfunction g(): void;"] });
rule("C04", "C", "low", "dts-doc-over-2-lines", DTS, /\/\*\*[\s\S]*?\*\//g,
    { on: "raw", key: "/**", when: (m) => (m[0].match(/\n/g) || []).length >= 2,
      hint: "rule 1: a d.ts doc comment is at most two short lines",
      pos: ["/** a\n *  b\n *  c */\nfunction f(): void;"], neg: ["/** a\n *  b */\nfunction f(): void;", "/** one */"] });
rule("C05", "C", "low", "doc-throws-on-promise", DTS, /\/\*\*([\s\S]*?)\*\/\s*((?:[^;{]|\{[^{}]*\})*?)[;{]/g,
    { on: "raw", key: "throws", when: (m) => /\bthrows?\b[^.;]*\b(?:I\/O|EIO|ENOENT|errno|network|connect\w*|read|write|open\w*|socket|disk|server|peer|timeout)\b/i.test(m[1]) && !/\breject/i.test(m[1]) && /\)\s*:\s*Promise\s*</.test(m[2]) && !/\b(?:synchronously|sync)\b/i.test(m[1]),
      hint: "doc says throws on a Promise-returning function: say rejects, or say the throw is synchronous",
      pos: ["/** Reads it; throws on I/O error. */\nfunction read(p: string): Promise<string>;"],
      neg: ["/** Reads it; rejects on I/O error. */\nfunction read(p: string): Promise<string>;", "/** Reads it; throws on I/O error. */\nfunction readSync(p: string): string;"] });
rule("F19", "F", "medium", "cwd-relative-write", TESTJS,
    /\b(?:(?:writeFile|appendFile|makeDir|mkdir)\s*\(\s*|(?:symlink|link|rename|copyFile)\s*\(\s*[^,()\n]+,\s*)(["'])(?![\/$~]|\.\.?\/)[^"'\n]+\1/g,
    { vpath: "tests/z/test_z.js", except: /^tests\/agent\//, hint: "a suite writes a cwd-relative path, which lands in the repo root: write under makeTempDir() or a $TMPDIR path",
      pos: ["os.symlink(\"x\", \"NULL\");", "await writeFile('out.txt', s);", "os.mkdir(\"scratch\");"],
      neg: ["os.symlink(\"seed.txt\", `${T}/l`);", "writeFile(\"/tmp/x\", s);", "os.mkdir(dir + \"/a\");", "os.symlink(\"seed.txt\", link);"] });
rule("E04", "E", "low", "fixed-sleep-sync", TESTJS,
    /(?:\bsetTimeout\s*\(\s*(?:r|res|resolve|done|ok|wake)\s*,\s*|(?<![\w.])(?:os\.)?(?:sleep|delay|wait)\s*\(\s*)(\d+)\s*\)/g,
    { except: /^tests\/agent\//, when: (m) => +m[1] >= 200, hint: "fixed sleep >= 200 ms used as a sync point: wait on the event instead",
      pos: ["await new Promise((r) => setTimeout(r, 500));", "os.sleep(300);", "await sleep(250);"],
      neg: ["await new Promise((r) => setTimeout(r, 50));", "setTimeout(() => { throw new Error('hang'); }, 5000);", "await sleep(10);"] });
rule("E05", "E", "low", "hard-coded-port", /^tests\/.*\.(?:js|mjs|sh)$/,
    /(?:\blisten\w*|\bbind|\bserve|create\w*Server|new\s+\w*Server|Server\.(?:listen|create|bind))\s*\(\s*(?:\{[^}\n]*?\bport\s*:\s*)?(\d{4,5})\b/g,
    { except: /^tests\/lib\/services\.js$|^tests\/agent\//, when: (m, f) => +m[1] >= 1024 && +m[1] <= 65535 && !/^\s*(?:\/\/|\*|#)/.test(f.text(f.line(m.index))),
      hint: "literal port outside the tests/lib/services.js allocator: collides under parallel runs",
      pos: ["const srv = net.listen({ port: 18080 });", "const s = serve({ host: '127.0.0.1', port: 9123 }, h);", "srv.listen(8080);"],
      neg: ["const srv = net.listen({ port: 0 });", "const p = PG_PORT;", "x = 'localhost:80';", "// listens on serve({ port: 9123 })", "await fetch('http://127.0.0.1:9123/x');", "TCPServer.connect({ host: 'h', port: 54321 });"] });

rule("F20", "F", "high", "zsh-unsafe-agent-script", /^\.agent-work\/.*\.(?:sh|bash)$/, /^#!.*\bzsh\b|\brsync\b|\bfor\s+\w+\s+in\s+\$\{?[A-Za-z_]\w*\}?(?=\s*;|\s*$|\s+do\b)/gm,
    { on: "raw", agent: true, vpath: ".agent-work/work9/tmp/z.sh", except: /\/(?:export|orig|[^\/]*\.bak|base|cutbase[^\/]*|keep[^\/]*|rec-[^\/]*|dry[^\/]*|b2)\//, when: (m, f) => !/^#!\s*(?:\/usr\/bin\/env\s+bash|\/opt\/homebrew\/bin\/bash)\b/.test(f.raw.slice(0, 80)) || /^#!.*\bzsh\b/.test(m[0]),
      hint: "agent script runs rsync or loops over an unquoted list variable without a bash shebang: under zsh the list is one word, so the loop runs once and rsync overwrites what it meant to keep; start it with #!/usr/bin/env bash or #!/opt/homebrew/bin/bash (zsh-only scripts are flagged)",
      pos: ["set -u\nrsync -a --files-from=- \$M/ \$W/", "for f in \$KEEP; do cp \$f k/; done", "#!/bin/zsh\necho hi", "#!/usr/bin/env zsh\nfor f in \$L\ndo :; done"],
      neg: ["#!/usr/bin/env bash\nrsync -a a b", "#!/opt/homebrew/bin/bash\nfor f in \$KEEP; do :; done", "for f in \"\${arr[@]}\"; do :; done", "for i in 1 2 3; do :; done", "echo rsyncing"] });
rule("F21", "F", "high", "hardcoded-service-port", /^tests\/.*\.js$/, /\b(?:55432|56379)\b/g,
    { vpath: "tests/postgres/test_x.js", except: /^tests\/lib\/services\.js$/, when: (m, f) => /from\s+["'][^"']*lib\/services\.js["']/.test(f.raw) && !/^\s*(?:\/\/|\*)/.test(f.text(f.line(m.index))),
      hint: "suite uses tests/lib/services.js but hardcodes its default port: run2 gives each parallel suite a private server on DYNA_TEST_PG_PORT / DYNA_TEST_REDIS_PORT, so import PG_PORT / REDIS_PORT (or use ensure*().conn / .port) instead",
      pos: ["import { ensurePostgres } from \"../lib/services.js\";\nconst PG_PORT = 55432;", "import { ensureRedis } from \"../lib/services.js\";\nconst r = new Redis({ port: 56379 });"],
      neg: ["import { ensurePostgres, PG_PORT } from \"../lib/services.js\";\nconst p = PG_PORT;", "const PG_PORT = 55432;", "import { ensureRedis } from \"../lib/services.js\";\n// redis on 56379"] });
rule("F01", "F", "low", "bin-bash-shebang", SH, /^#!\s*\/bin\/bash\b/g,
    { on: "raw", hint: "macOS /bin/bash is 3.2: use #!/usr/bin/env bash", pos: ["#!/bin/bash\necho hi"], neg: ["#!/usr/bin/env bash\necho hi", "echo x\n#!/bin/bash"] });
rule("F02", "F", "med", "zsh-ism", SH, /^\s*(?:setopt|unsetopt|zmodload|autoload\s+-U)\b|\$\{\([^)\s]+\)|\bprint\s+-P\b/gm,
    { hint: "zsh syntax in a bash script", pos: ["setopt nullglob", "echo ${(j:,:)arr}"], neg: ["shopt -s nullglob", "echo ${arr[@]}"] });
rule("F03", "F", "low", "large-seq", SH, /\bseq\s+(?:-\w+\s+\S+\s+)*(?:\d+\s+){0,2}(\d{6,})\b/g,
    { when: (m) => +m[1] >= 100000, hint: "seq over 1e5+ values: use a bash arithmetic loop or a dynajs script",
      pos: ["for i in $(seq 1 1000000); do :; done"], neg: ["for i in $(seq 1 100); do :; done"] });
rule("F04", "F", "med", "head-negative", SH, /\bhead\s+(?:-n\s*-\d+|--lines=-\d+)/g,
    { hint: "head -n -N is GNU-only (fails on macOS)", pos: ["ls | head -n -2"], neg: ["ls | head -n 2", "ls | head -5"] });
rule("F05", "F", "med", "sed-i-no-backup", SH, /\bsed\s+(?:-[a-zA-Z]+\s+)*-[a-zA-Z]*i(?=\s+(?:-e\b|-E\b|['"][^'"\s]*[\/|#,!]))/g,
    { except: LINUXONLY, hint: "sed -i without a suffix differs GNU vs BSD: use -i.bak and remove, or a temp file",
      pos: ["sed -i 's/a/b/' f", "sed -E -i -e 's/a/b/' f"], neg: ["sed -i.bak 's/a/b/' f", "sed -i '' 's/a/b/' f", "sed 's/a/b/' f"] });
rule("F06", "F", "med", "tmp-path", /(?:\.(?:sh|bash)$|^tools\/.*\.(?:js|mjs)$|^tests\/.*\.(?:js|mjs)$)/, /(?<![\w.$\/{}-])(?<!\+\s*["'`])\/tmp(?=\/|\b)/g,
    { on: "raw", except: /^tools\/sweep\.js$|(?:^|\/)(?:docker|android)\/|Dockerfile/, hint: "rule 7: never /tmp; use a work dir or TMPDIR under it",
      pos: ['out=/tmp/x.log', 'const p = "/tmp/sock";'], neg: ['out=$TMPDIR/x.log', 'const p = root + "/tmp/sock";', 'rm -rf ./tmp/t'] });
rule("F07", "F", "med", "python", /\.(?:sh|bash|js|mjs)$/, /^#!.*\bpython|(?<![\w.-])python3?(?=\s+(?:-[A-Za-z]|\S+\.py\b))/gm,
    { key: "python", except: SELF, hint: "rule 8: never Python", pos: ["python3 -c 'print(1)'", "#!/usr/bin/env python3"], neg: ["echo pythonic", "# python-free"] });
rule("F08", "F", "high", "status-masked", SH,
    /^[ \t]*(?:local|export|declare|readonly|echo|printf)\b[^\n]*\$\([^\n]*\n[^\n]*\$\?|(?:local|export|declare|readonly)\s+\w+=\$\([^\n]*?\)\s*;[^\n]*\$\?|\$\([^\n]*\)\s*\|[^\n]*;\s*\w+=\$\?/gm,
    { on: "raw", key: "$?", hint: "$? read after local/export/echo/pipe around $(...): it is that wrapper's status, not the command's",
      pos: ["local out=$(run)\nrc=$?", "echo \"$(run)\"\n[ $? -eq 0 ]", "x=$(run) | tee f; rc=$?"],
      neg: ["out=$(run)\nrc=$?", "local out\nout=$(run); rc=$?"] });
rule("F16", "F", "high", "status-after-subst-same-line", SH, /^[^\n]*\$\?[^\n]*$/gm,
    { on: "raw", key: "$?", when: (m) => statusAfterSubst(m[0]),
      hint: "$? read on the same line after a $(...) that is not a plain v=$(...); statement: it is the status of the wrapper (echo, local, ||, a pipe), not of the command",
      pos: ["echo \"$(run)\"; rc=$?", "out=$(run) || true; rc=$?", "echo \"got $(run) rc=$?\"", "x=$(run) | tee f; rc=$?", "local o=$(run); rc=$?"],
      neg: ["out=$(run); rc=$?", "rc=$?; out=$(next)", "n=$((n+1)); rc=$?", "echo '$(x) $?'", "[ $? -eq 0 ] && v=$(f)", "out=\"$(run 2>&1)\"; rc=$?"] });
rule("F10", "F", "med", "grep-P", SH, /\bgrep\s+(?:-[a-zA-Z]*\s+)*-[a-zA-Z]*P/g,
    { except: LINUXONLY, hint: "macOS grep has no -P: use -E or dynajs", pos: ["grep -P '\\d+' f", "grep -oP 'x' f"], neg: ["grep -E '[0-9]+' f"] });
rule("F11", "F", "med", "date-nanos", SH, /\bdate\b[^\n|;]*\+[^\s|;]*%N/g,
    { except: LINUXONLY, hint: "BSD date has no %N: time with dynajs or $EPOCHREALTIME", pos: ["t=$(date +%s%N)"], neg: ["t=$(date +%s)", "t=$(gdate +%s%N)"] });
rule("F12", "F", "med", "stat-c", SH, /\bstat\s+-c\b/g,
    { except: LINUXONLY, unless: { after: 0, re: /stat\s+-f/ }, hint: "stat -c is GNU-only: add the BSD stat -f fallback",
      pos: ["sz=$(stat -c %s f)"], neg: ["sz=$(stat -f %z f 2>/dev/null || stat -c %s f)"] });
rule("F13", "F", "med", "find-printf", SH, /\bfind\b[^\n|;]*\s-printf\b/g,
    { except: LINUXONLY, hint: "BSD find has no -printf", pos: ["find . -name x -printf '%p\\n'"], neg: ["find . -name x -print"] });
rule("F14", "F", "low", "nproc", SH, /(?<![\w-])nproc\b/g,
    { except: LINUXONLY, unless: { after: 0, re: /sysctl|getconf|\|\||command -v/ }, hint: "nproc is absent on stock macOS: fall back to sysctl -n hw.ncpu",
      pos: ["j=$(nproc)"], neg: ["j=$(nproc 2>/dev/null || sysctl -n hw.ncpu)"] });
rule("F15", "F", "med", "timeout-cmd", SH, /(?:^|[;&|(]\s*|\$\(\s*)timeout\s+-?[\dk]/gm,
    { except: LINUXONLY, unless: { scope: "file", re: /^\s*timeout\s*\(\)|gtimeout|command -v timeout|type timeout/m },
      hint: "timeout(1) is GNU coreutils, absent on stock macOS", pos: ["timeout 5 ./x"], neg: ["timeout() { perl -e 'alarm shift; exec @ARGV' \"$@\"; }\ntimeout 5 ./x"] });

const REMOVED = [
    ["--memory-limit", /--memory-limit\b/], ["JS_SetMemoryLimit", /\bJS_SetMemoryLimit\b/], ["dynajsc", /\bdynajsc\b/],
    ["JS_DumpMemoryUsage", /\bJS_DumpMemoryUsage\b/], ["js_std_eval_binary", /\bjs_std_eval_binary\w*/],
    ["runner-flag", /\bdynajs\b[^\n]*(?:^|[\s"'\[,])(?:--trace|--dump|--quit|--include|--strip-source|--script|--strict)(?=[\s"',\]]|$)/i],
];
rule("G01", "G", "med", "removed-surface", /^(?:src|tools|tests|examples|bench)\/|^README\.md$|^build\.sh$|\.d\.ts$/,
    new RegExp(REMOVED.map(([, r]) => "(?:" + r.source + ")").join("|"), "gim"),
    { on: "raw", except: SELF, key: ["memory-limit", "SetMemoryLimit", "dynajsc", "DumpMemoryUsage", "eval_binary", "--trace", "--dump", "--quit", "--include", "--strip-source", "--script", "--strict"], hint: "removed surface (remove-means-delete-code): delete the leftover",
      pos: ["./dynajs --memory-limit 1m x.js", "JS_SetMemoryLimit(rt, 0);", "see dynajsc output", "Exec(\"./dynajs\", [\"--strict\", f])"],
      neg: ["./dynajs x.js", "memory limit", "node --strict x.js"] });

function stmtStart(f, idx) {
    let i = idx - 1;
    const c = f.code;
    while (i >= 0 && /\s/.test(c[i])) i--;
    if (i < 0 || ";{}:".includes(c[i])) return true;
    if (/\belse$/.test(c.slice(Math.max(0, i - 4), i + 1))) return true;
    return c[i] === ")" && /^\s*(?:\}\s*)?(?:else\s+)?(?:if|while|for)\b/.test(lineText(c, f.ls, f.line(i)));
}
const CFUNC_BOUNDARY = new Set(["js_call_c_function", "js_call_cfunc_generic", "js_c_function_data_call", "js_iterator_step"]);
function inWin32(code, idx) {
    const st = [];
    const re = /^[ \t]*#[ \t]*(ifdef|ifndef|if|elif|else|endif)\b([^\n]*)/gm;
    let m;
    while ((m = re.exec(code)) && m.index < idx) {
        const k = m[1], c = m[2];
        const has = /\b_WIN32\b/.test(c);
        const neg = has && (k === "ifndef" || /!\s*defined\s*\(?\s*_WIN32/.test(c));
        if (k === "if" || k === "ifdef" || k === "ifndef") st.push({ w: has && !neg, n: neg });
        else if (k === "elif") { if (st.length) st[st.length - 1] = { w: has && !neg, n: false }; }
        else if (k === "else") { if (st.length) { const t = st[st.length - 1]; st[st.length - 1] = { w: t.n, n: false }; } }
        else if (k === "endif") st.pop();
    }
    return st.some((t) => t.w);
}
function fnExcRestore(fn, body) {
    if (/=\s*[\w>.\-]*current_exception_is_uncatchable\b/.test(body) && /current_exception_is_uncatchable\s*=(?!=)/.test(body)) return [];
    const out = [];
    const re = /->current_exception\s*=\s*([A-Za-z_]\w*)\s*;/g;
    let m;
    while ((m = re.exec(body))) {
        if (/^JS_[A-Z_]+$/.test(m[1])) continue;
        if (!new RegExp("\\b" + m[1] + "\\s*=\\s*[\\w>.\\-]*->current_exception\\s*;").test(body)) continue;
        out.push({ off: m.index, note: "restores " + m[1] + " without current_exception_is_uncatchable" });
    }
    return out;
}
function statusAfterSubst(line) {
    if (/^\s*#/.test(line)) return false;
    const s = line.replace(/'[^']*'/g, "''").replace(/\$\(\((?:[^()]|\([^()]*\))*\)\)/g, "0");
    const segs = [], seps = [];
    let d = 0, cur = "";
    for (let i = 0; i < s.length; i++) {
        const c = s[i], two = s.slice(i, i + 2);
        if (two === "$(") { d++; cur += two; i++; continue; }
        if (c === "(" && d > 0) { d++; cur += c; continue; }
        if (c === ")" && d > 0) { d--; cur += c; continue; }
        if (d === 0 && (two === "&&" || two === "||")) { segs.push(cur); seps.push(two); cur = ""; i++; continue; }
        if (d === 0 && c === ";") { segs.push(cur); seps.push(";"); cur = ""; continue; }
        cur += c;
    }
    segs.push(cur);
    const pure = /^\s*[A-Za-z_]\w*=("?)\$\((?:[^()]|\([^()]*\))*\)\1\s*$/;
    for (let k = 0; k < segs.length; k++) {
        const q = segs[k].indexOf("$?");
        if (q < 0) continue;
        const sub = segs[k].indexOf("$(");
        if (sub >= 0 && sub < q) return true;
        for (let j = 0; j < k; j++) {
            if (!segs[j].includes("$(")) continue;
            if (j === k - 1 && seps[j] === ";" && pure.test(segs[j])) continue;
            return true;
        }
    }
    return false;
}
function issuesAfterQueue(text, horizonDays) {
    const lines = text.split("\n");
    const q = lines.findIndex((l) => /^## QUEUE\b/.test(l));
    if (q < 0) return [];
    let end = lines.findIndex((l, i) => i > q && /^## /.test(l));
    if (end < 0) return [];
    const day = (d) => Date.parse(d + "T00:00:00Z");
    let max = 0;
    for (const m of text.matchAll(/\b(20\d\d-\d\d-\d\d)\b/g)) max = Math.max(max, day(m[1]) || 0);
    const out = [];
    for (let i = end; i < lines.length; i++) {
        const l = lines[i];
        if (!/^(?:#{2,3} |[A-Z][A-Z0-9 -]{2,}:)/.test(l)) continue;
        const m = /\b(20\d\d-\d\d-\d\d)\b/.exec(l);
        if (m && day(m[1]) >= max - horizonDays * 86400000) out.push({ line: i + 1, text: l.slice(0, 110) });
    }
    return out;
}
function cLitLen(s) {
    let n = 0;
    for (let i = 0; i < s.length; i++, n++) {
        if (s[i] !== "\\") continue;
        const c = s[++i];
        if (c === "x") { while (i + 1 < s.length && /[0-9a-fA-F]/.test(s[i + 1])) i++; }
        else if (/[0-7]/.test(c)) { let k = 0; while (k < 2 && i + 1 < s.length && /[0-7]/.test(s[i + 1])) { i++; k++; } }
    }
    return n;
}

function fnAllocMismatch(fn, body) {
    const kinds = new Map();
    const re = /\b([A-Za-z_]\w*(?:->\w+)?)\s*=\s*(?:\([^()]*\)\s*)?(js_malloc\w*|js_realloc\w*|js_strdup\w*|js_strndup|malloc|calloc|realloc|strdup|strndup)\s*\(/g;
    let m;
    while ((m = re.exec(body))) {
        const js = m[2].startsWith("js_");
        const prev = kinds.get(m[1]);
        kinds.set(m[1], prev === undefined ? js : prev === js ? js : null);
    }
    const out = [];
    for (const [v, js] of kinds) {
        if (js === null) continue;
        const fre = js ? new RegExp("(?<![\\w.>])free\\s*\\(\\s*" + esc(v) + "\\s*\\)") : new RegExp("\\bjs_free\\w*\\s*\\(\\s*\\w+\\s*,\\s*" + esc(v) + "\\s*\\)");
        const h = fre.exec(body);
        if (h) out.push({ off: h.index, note: v + (js ? " from js_* alloc, freed by free()" : " from libc alloc, freed by js_free()") });
    }
    return out;
}
function fnUnsignedNeg(fn, body) {
    if (!/(?:<|>=)\s*0(?![\w.x])|(?<![\w.])0\s*>/.test(body)) return [];
    const vars = new Set();
    const decl = /\b(?:size_t|unsigned(?:\s+(?:int|long(?:\s+long)?|char|short))?|uint(?:8|16|32|64)_t|uintptr_t)\s+(?!\*)([A-Za-z_]\w*)\s*(?=[,;=)\[]|$)/g;
    for (const t of [fn.params, body]) { let d; decl.lastIndex = 0; while ((d = decl.exec(t))) vars.add(d[1]); }
    const sdecl = /\b(?:ssize_t|int|long|int(?:8|16|32|64)_t|off_t|intptr_t|ptrdiff_t|double|float)\s+\**([A-Za-z_]\w*)\s*(?=[,;=)\[]|$)/g;
    for (const t of [fn.params, body]) { let d; sdecl.lastIndex = 0; while ((d = sdecl.exec(t))) vars.delete(d[1]); }
    if (!vars.size) return [];
    const re = /(?<![\w.>)])([A-Za-z_]\w*)\s*(?:<\s*0(?![\w.x])|>=\s*0(?![\w.x]))|(?<![\w.])0\s*>\s*([A-Za-z_]\w*)(?![\w(])/g;
    const out = [];
    let m;
    while ((m = re.exec(body))) { const v = m[1] || m[2]; if (vars.has(v)) out.push({ off: m.index, note: v + " is unsigned" }); }
    return out;
}
function fnForkExit(fn, body) {
    if (!/(?<![\w.>])v?fork\s*\(/.test(body)) return [];
    const m = /(?<![\w.>])exit\s*\(/.exec(body);
    return m ? [{ off: m.index }] : [];
}

rule("F17", "F", "med", "long-list-line", /^(?:tests|tools)\/.*\.(?:sh|bash|tsv)$/, /^[^\n]{401,}$/gm,
    { on: "raw", vpath: "tools/z.sh", when: (m, f) => f.path.endsWith(".tsv") || /^\s*(?:export\s+)?[A-Za-z_]\w*=["'(]|\bcase\b[^\n]*\bin\s+[\w-]+(?:\|[\w-]+){4,}|\bfor\s+\w+\s+in\s+\S+(?:\s+\S+){9,}/.test(m[0]),
      hint: "list on one line over 400 chars: two patches that each add an item conflict and a stale hunk silently drops one; put one item per line (a multi-line \"...\" string or array)",
      pos: ["T=\"" + "test-a ".repeat(70) + "\"", "  ( case $t in " + "test-a|".repeat(70) + "test-b) x ;; esac", "for t in " + "a-test ".repeat(70) + "; do :; done"],
      neg: ["T=\"\ntest-a\ntest-b\n\"", "    printf -- '" + "\\000".repeat(120) + "' > f", "echo " + "x".repeat(500)] });

rule("F18", "F", "high", "conflict-marker", /^(?:src|tests|tools|examples|bench|docker)\/.*\.(?:c|h|js|mjs|ts|sh|bash|tsv|json|toml|ya?ml|txt|py)$|^[^/]+\.(?:sh|js|ts|c|h)$/, /^(?:<{7} \S[^\n]*|={7}|>{7} \S[^\n]*)$/gm,
    { on: "raw", vpath: "tools/z.sh",
      hint: "merge conflict marker left by a sync or git apply --3way: the file no longer parses or silently keeps both sides",
      pos: ["a\n<<<<<<< ours\nb\n=======\nc\n>>>>>>> theirs\n", "x\n=======\ny\n"],
      neg: ["s = '<<<<<<< ours';\n", "echo =======x\n", "# ======== heading\n"] });

const JOINS = [];
const join = (id, cat, sev, name, run, o = {}) => JOINS.push({ id, cat, sev, name, run, ...o });

join("B01", "B", "high", "pool-work-touches-js", jPoolJs, {
    hint: "pool/offload work callback runs off the JS thread but touches JS_*/JSContext/ctx->",
    pos: [{ "src/core/x.c": "static void w(void* a){\n    JSContext* ctx = a;\n    JS_FreeValue(ctx, v);\n}\nstatic void d(void* a){}\nint s(dyn_aio_t* io, void* c){\n    return dyn_aio_offload(io, w, d, c);\n}\n" }],
    neg: [{ "src/core/x.c": "static void w(void* a){\n    struct job* j = a;\n    j->n = compute(j->buf);\n}\nstatic void d(void* a){ JS_FreeValue(ctx, v); }\nint s(dyn_aio_t* io, void* c){\n    return dyn_aio_offload(io, w, d, c);\n}\n" }],
});
join("B02", "B", "info", "cfunc-sync-codec", jCfuncCodec, {
    hint: "JS-thread C function runs codec/crypto synchronously: P-OFFMAIN audit input",
    pos: [{ "src/modules/z/z-a.c": "static JSValue z_hash(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv){\n    EVP_DigestUpdate(md, p, n);\n    return JS_UNDEFINED;\n}\n" }],
    neg: [{ "src/modules/z/z-a.c": "static JSValue z_len(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv){\n    return JS_NewInt32(ctx, 3);\n}\n" }],
});
join("C01", "C", "med", "exported-undeclared", jExportDecl, {
    hint: "C exports a name the module d.ts never mentions",
    pos: [{ "src/modules/z/z.d.ts": "declare module \"dyna:z\" {\n    function a(x: number): number;\n}\n", "src/modules/z/z-m.c": "static const JSCFunctionListEntry e[] = {\n    JS_CFUNC_DEF(\"a\", 1, za),\n    JS_CFUNC_DEF(\"hidden\", 0, zh),\n};\n" }],
    neg: [{ "src/modules/z/z.d.ts": "declare module \"dyna:z\" {\n    function a(x: number): number;\n}\n", "src/modules/z/z-m.c": "static const JSCFunctionListEntry e[] = {\n    JS_CFUNC_DEF(\"a\", 1, za),\n};\n" }],
});
join("C02", "C", "med", "declared-unimplemented", null, { hint: "d.ts declares a function/method no module source names", via: "C01" });
join("C03", "C", "low", "arity-mismatch", null, { hint: "JS_CFUNC length outside the d.ts [required..total] parameter range", via: "C01" });
join("D01", "D", "low", "rule18-gap", jRule18, {
    hint: "module tests lack rule-18 properties (see rule18.tsv)",
    pos: [{ "tools/modules.tsv": "module\tkind\texports\tsrc\tkits\tpkg\tdeps\ttests\tdescription\nz\tmodule\tdyna:z\tsrc/modules/z/\t\t\t\ttests/z\td\n", "tests/z/test_z.js": "assert(1);\n" }],
    neg: [{ "tools/modules.tsv": "module\tkind\texports\tsrc\tkits\tpkg\tdeps\ttests\tdescription\nz\tmodule\tdyna:z\tsrc/modules/z/\t\t\t\ttests/z\td\n",
        "tests/z/test_z.js": "new Proxy({}, {}); f('a\\0b'); f(Number.MAX_SAFE_INTEGER); gc(); setInterruptHandler();\n", "tests/z/test_z_oom.c": "static int z_fail_at;\n", "src/fuzz/fuzz_z.c": "int LLVMFuzzerTestOneInput(){ return 0; }\n" },
        { "tools/modules.tsv": "module\tkind\texports\tsrc\tkits\tpkg\tdeps\ttests\tdescription\nz\tmodule\tdyna:z\tsrc/modules/z/\tsrc/core/wz/*.c !src/core/wz/{x,y}.c\t\t\ttests/z\td\n",
        "tests/z/test_z.js": "new Proxy({}, {}); f('a\\0b'); f(Number.MAX_SAFE_INTEGER); gc(); setInterruptHandler();\n", "tests/z/test_z_oom.c": "static int z_fail_at;\n", "src/fuzz/fuzz_wirez.c": "#include \"model.h\"\nint LLVMFuzzerTestOneInput(){ return 0; }\n", "src/core/wz/wirez.c": "int a;\n", "src/core/wz/x.c": "int b;\n" }],
});
join("E01", "E", "high", "ctest-not-run", jCtest, {
    hint: "C test with main() that no tests/ctest/run.sh recipe in TARGETS builds",
    pos: [{ "tests/ctest/run.sh": "TARGETS=\"test-a\"\ncase \"$1\" in\n  test-a)\n    cc tests/x/test_a.c ;;\nesac\n", "tests/x/test_a.c": "int main(void){return 0;}\n", "tests/x/test_b.c": "int main(void){return 0;}\n" }],
    neg: [{ "tests/ctest/run.sh": "TARGETS=\"test-a\"\ncase \"$1\" in\n  test-a)\n    cc tests/x/test_a.c ;;\nesac\n", "tests/x/test_a.c": "int main(void){return 0;}\n", "tests/x/helper.c": "int h(void){return 0;}\n" }, { "tests/ctest/run.sh": "TARGETS=\"\ntest-a\ntest-b\n\"\ncase \"$1\" in\n  test-a)\n    cc tests/x/test_a.c ;;\n  test-b)\n    cc tests/x/test_b.c ;;\nesac\n", "tests/x/test_a.c": "int main(void){return 0;}\n", "tests/x/test_b.c": "int main(void){return 0;}\n" }],
});
join("E02", "E", "high", "ctest-target-broken", null, { hint: "TARGETS entry without a case recipe, or a recipe naming a missing .c", via: "E01" });
join("E03", "E", "med", "suite-no-assert", jSuites, {
    hint: "suite has no assertion, throw or failing exit: it can only pass",
    pos: [{ "tests/z/test_z.js": "console.log(1 + 1);\n", "tests/known-unrun.txt": "" }],
    neg: [{ "tests/z/test_z.js": "if (1 + 1 !== 2) throw new Error('x');\n", "tests/z/test_y_child.js": "console.log(1);\n", "tests/known-unrun.txt": "" }],
});
join("E06", "E", "med", "service-no-stop", null, { hint: "starts a service via services.js but never stops it", via: "E03" });
join("E08", "E", "med", "helper-picked-as-suite", null, { hint: "file named like a suite is imported or spawned by another test: run2 runs it as a suite", via: "E03" });
join("E07", "E", "low", "allowlist-stale", jLists, {
    hint: "list entry names a missing file, or a std/os-subject entry that no longer uses std/os",
    pos: [{ "tests/std_os_subject.txt": "tests/z/test_gone.js\ntests/z/test_plain.js\n", "tests/z/test_plain.js": "import * as f from 'dyna:file';\n", "tests/known-unrun.txt": "tests/z/test_missing.js\tdead\n" }],
    neg: [{ "tests/std_os_subject.txt": "tests/z/test_os.js\n", "tests/z/test_os.js": "import * as os from \"os\";\n", "tests/known-unrun.txt": "tests/z/test_os.js\tslow\n" }],
});
join("F07b", "F", "med", "python-file", jPython, {
    hint: "rule 8: Python file or __pycache__ in the tree",
    pos: [{ "tools/x.py": "print(1)\n" }, { "tools/__pycache__/x.cpython-312.pyc": "" }], neg: [{ "tools/x.js": "console.log(1)\n" }],
});
join("H01", "H", "info", "run-list-from-log", jRunFromLog, {
    hint: "advisory: run2/merge argument list built from a log; derive it from the patch (git diff --name-only) or tests/suites.sh",
    pos: [{ "tools/m.sh": "s=$(grep -o 'tests/[^ ]*' run.log | sort -u)\nbash ../manual-run/run2.sh $s\n" }, { "tools/m.sh": "bash run2.sh $(grep FAIL logs/x.log | cut -f1)\n" }],
    neg: [{ "tools/m.sh": "s=$(git diff --name-only base -- tests)\nbash run2.sh $s\n" }, { "tools/m.sh": "grep FAIL x.log\nbash run2.sh tests/a/test_a.js\n" }],
});
join("H02", "H", "med", "coverage-map-stale", jCovMap, {
    hint: "run2 selects suites from .obj/cov/map.tsv; a map older than 20 merges (merges.tsv rows past the header's merges=, else past its built day) misses their call paths",
    pos: [{ ".obj/cov/map.tsv": "# coverage-map v1\thead=h\tbuilt=1795626156\tmerges=5\n", ".agent-work/merges.tsv": "date\tpatch\n" + Array.from({ length: 26 }, (_, i) => "2026-11-20\tp" + i + "\tgreen\n").join("") },
        { ".obj/cov/map.tsv": "# coverage-map v1\thead=h\tbuilt=1795626156\n", ".agent-work/merges.tsv": "date\tpatch\n" + Array.from({ length: 21 }, (_, i) => "2026-11-28\tp" + i + "\tgreen\n").join("") },
        { ".obj/cov/map.tsv": "# coverage-map v1\thead=h\n" }],
    neg: [{ ".obj/cov/map.tsv": "# coverage-map v1\thead=h\tbuilt=1795626156\tmerges=10\n", ".agent-work/merges.tsv": "date\tpatch\n" + Array.from({ length: 30 }, (_, i) => "2026-11-28\tp" + i + "\tgreen\n").join("") },
        { ".obj/cov/map.tsv": "# coverage-map v1\thead=h\tbuilt=" + Math.floor(new Date(2026, 10, 28, 0, 30).getTime() / 1000) + "\n", ".agent-work/merges.tsv": "date\tpatch\n" + Array.from({ length: 30 }, (_, i) => "2026-11-28\tp" + i + "\tgreen\n").join("") },
        { ".obj/cov/map.tsv": "# coverage-map v1\thead=h\tbuilt=1795626156\n", ".agent-work/merges.tsv": "date\tpatch\n" + Array.from({ length: 20 }, (_, i) => "2026-11-28\tp" + i + "\tgreen\n").join("") + "2026-11-20\told\tgreen\n" },
        { "src/a.c": "int a;\n" }],
});
join("F09", "F", "med", "brew-no-stop", jBrew, {
    hint: "brew services started without a matching stop in the same script (rule 15)",
    pos: [{ "tools/a.sh": "brew services start redis\n./run\n" }], neg: [{ "tools/a.sh": "brew services start redis\n./run\nbrew services stop redis\n" }],
});

function fnIndex(S) {
    if (S._fnIndex) return S._fnIndex;
    const idx = new Map();
    for (const f of S.ctxs) if (CSRC.test(f.path)) for (const fn of f.fns) { if (!idx.has(fn.name)) idx.set(fn.name, []); idx.get(fn.name).push({ f, fn }); }
    return (S._fnIndex = idx);
}
function jPoolJs(S, emit) {
    const call = /\b(dyn_pool_submit(?:_cpu)?|dyn_aio_offload(?:_cpu)?)\s*\(\s*(?:[^,()]|\([^()]*\))+,\s*([A-Za-z_]\w*)\s*,/g;
    const touch = /\bJS_[A-Z]\w*\s*\(|\bJSContext\b|\bctx\s*->|\bJSValue\b/;
    const seen = new Set();
    for (const f of S.ctxs) {
        if (!CSRC.test(f.path) || !/dyn_(?:pool_submit|aio_offload)/.test(f.raw)) continue;
        let m;
        call.lastIndex = 0;
        while ((m = call.exec(f.code))) {
            const name = m[2];
            const local = f.fns.filter((x) => x.name === name);
            const defs = local.length ? local.map((fn) => ({ f, fn })) : fnIndex(S).get(name) || [];
            for (const { f: df, fn } of defs) {
                const key = df.path + ":" + name;
                if (seen.has(key)) continue;
                seen.add(key);
                const body = df.code.slice(fn.open, fn.close);
                const t = touch.exec(body);
                if (t) emit("B01", df, fn.open + t.index, "work fn " + name + " (submitted at " + f.path + ":" + (f.line(m.index) + 1) + ") touches " + t[0].trim());
            }
        }
    }
}
function jCfuncCodec(S, emit) {
    const sig = /\(\s*JSContext\s*\*\s*\w+\s*,\s*JSValueConst\s+\w+\s*,\s*int\s+\w+\s*,\s*JSValueConst\s*\*\s*\w+/;
    const codec = /\b(inflate\w*|deflate\w*|EVP_(?:Digest|Cipher|Encrypt|Decrypt|PKEY_sign|PKEY_verify|DigestSign|DigestVerify)\w*|SHA(?:1|224|256|384|512)(?:_Update|_Final)?|HMAC(?:_Update)?|PKCS5_\w+|BZ2_\w+|ZSTD_(?:de)?compress\w*|LZ4_\w+|Brotli\w+|\w+_(?:decompress|compress|encrypt|decrypt|inflate|deflate))\s*\(/;
    for (const f of S.ctxs) {
        if (!/^src\/modules\/.*\.c$/.test(f.path)) continue;
        for (const fn of f.fns) {
            if (!sig.test(fn.hdr)) continue;
            const body = f.code.slice(fn.open, fn.close);
            const c = codec.exec(body);
            if (c) emit("B02", f, fn.open + c.index, fn.name + " calls " + c[1] + " on the JS thread");
        }
    }
}
function parseParams(text, open) {
    let d = 0, i = open, start = open + 1;
    const parts = [];
    for (; i < text.length; i++) {
        const c = text[i];
        if (c === "(" || c === "[" || c === "{") d++;
        else if (c === "<" && text[i - 1] !== "=") d++;
        else if (c === ")" || c === "]" || c === "}") { d--; if (d === 0) { parts.push(text.slice(start, i)); break; } }
        else if (c === ">" && text[i - 1] !== "=") d--;
        else if (c === "," && d === 1) { parts.push(text.slice(start, i)); start = i + 1; }
    }
    let req = 0, total = 0, rest = false, opt = false;
    for (let p of parts) {
        p = p.trim();
        if (!p || /^this\s*:/.test(p)) continue;
        if (p.startsWith("...")) { rest = true; continue; }
        total++;
        if (/^[\w$]+\s*\?/.test(p) || /=/.test(p.replace(/=>/g, ""))) opt = true;
        if (!opt) req++;
    }
    return { req, total, rest };
}
function jExportDecl(S, emit) {
    let all = null;
    const allSrc = () => all !== null ? all : (all = S.ctxs.filter((f) => /^src\/.*\.[ch]$/.test(f.path)).map((f) => f.raw).join("\n"));
    const typesTok = new Set();
    for (const f of S.ctxs) if (/^src\/types\/.*\.d\.ts$/.test(f.path)) for (const t of stripTsComments(f.raw).match(/[A-Za-z_$][\w$]*/g) || []) typesTok.add(t);
    const mods = new Map();
    for (const f of S.ctxs) {
        const m = /^src\/modules\/([^/]+)\//.exec(f.path);
        if (!m) continue;
        if (!mods.has(m[1])) mods.set(m[1], { dts: null, dtsAll: [], srcs: [] });
        const e = mods.get(m[1]);
        if (f.path === "src/modules/" + m[1] + "/" + m[1] + ".d.ts") e.dts = f;
        if (/\.d\.ts$/.test(f.path)) e.dtsAll.push(f); else if (/\.(?:c|h|js)$/.test(f.path)) e.srcs.push(f);
    }
    const defRe = /\bJS_(?:CFUNC_(?:MAGIC_)?DEF|CGETSET_(?:MAGIC_)?DEF|ITERATOR_NEXT_DEF|PROP_\w+_DEF|ALIAS_DEF)\s*\(\s*"([^"]+)"(?:\s*,\s*(\d+))?/g;
    for (const [mod, e] of mods) {
        if (!e.dts) continue;
        const dts = stripTsComments(e.dts.raw);
        const tok = new Set(e.dtsAll.flatMap((f) => stripTsComments(f.raw).match(/[A-Za-z_$][\w$]*/g) || []));
        const srcText = e.srcs.map((f) => f.raw).join("\n");
        const lens = new Map();
        for (const f of e.srcs) {
            if (!/\.c$/.test(f.path)) continue;
            let m;
            defRe.lastIndex = 0;
            while ((m = defRe.exec(f.raw))) {
                const name = m[1];
                if (/^\[|^Symbol\.|^(?:next|return|throw)$/.test(name)) continue;
                if (!tok.has(name) && !typesTok.has(name)) emit("C01", f, m.index, mod + ": \"" + name + "\" exported, not in any " + mod + " d.ts (outside comments)");
                if (m[2] !== undefined && /CFUNC/.test(m[0])) { if (!lens.has(name)) lens.set(name, []); lens.get(name).push({ f, idx: m.index, n: +m[2] }); }
            }
        }
        const decl = /^[ \t]*(?:export\s+)?(?:declare\s+)?(?:static\s+)?(?:async\s+)?(?:function\s+)?([A-Za-z_$][\w$]*)\s*(?:<(?:[^<>]|<[^<>]*>)*>)?\s*\(/gm;
        const sigs = new Map();
        let m;
        while ((m = decl.exec(dts))) {
            const name = m[1];
            if (/^(?:if|for|while|switch|return|new|constructor|typeof|keyof|function)$/.test(name)) continue;
            const open = m.index + m[0].length - 1;
            if (!sigs.has(name)) sigs.set(name, []);
            sigs.get(name).push({ idx: m.index, ...parseParams(dts, open), fn: /\bfunction\s/.test(m[0]) });
        }
        for (const [name, list] of sigs) {
            if (!list.some((s) => s.fn)) continue;
            const lit = new RegExp("\"" + esc(name) + "\"|\\bfunction\\s+" + esc(name) + "\\b|\\b" + esc(name) + "\\s*[:(=]");
            if (!lit.test(srcText) && !allSrc().includes("\"" + name + "\"")) emit("C02", e.dts, list[0].idx, mod + ": " + name + "() declared, no source names it");
        }
        for (const [name, uses] of lens) {
            const list = sigs.get(name);
            if (!list) continue;
            const minReq = Math.min(...list.map((s) => s.req)), maxTot = Math.max(...list.map((s) => s.total)), rest = list.some((s) => s.rest);
            for (const u of uses) if (u.n < minReq || (u.n > maxTot && !rest)) emit("C03", u.f, u.idx, mod + ": " + name + " length " + u.n + ", d.ts params " + minReq + ".." + maxTot + (rest ? "+rest" : ""));
        }
    }
}
const R18 = [
    ["hostile", /new Proxy\b|\bvalueOf\s*[(:]|\btoString\s*[(:]|\[Symbol\.toPrimitive\]|\bget\s+\w+\s*\(\)\s*\{[^}]*throw/],
    ["nul", /\\0(?![0-9])|\\x00|\\u0000|\\u\{0\}|fromCharCode\(\s*0\s*\)/],
    ["limits", /MAX_SAFE_INTEGER|2\s*\*\*\s*(?:31|32|53)|0x7fffffff|0xffffffff|2147483647|4294967295|Number\.MAX_VALUE|SIZE_MAX|INT_MAX/i],
    ["mallocfail", /fail_at\b|_fail_at|malloc_fail|alloc_fail|fail_after|fault_inject|failing_malloc/i],
    ["gc", /\bgc\s*\(\s*\)|FinalizationRegistry|\bWeakRef\b|JS_RunGC/],
    ["interrupt", /interrupt/i],
];
function kitIn(list, f) {
    let hit = false;
    for (const e of list) {
        const neg = e.startsWith("!"), p = neg ? e.slice(1) : e;
        const m = p.endsWith("/") ? f.startsWith(p) : /[*{]/.test(p) ? new RegExp("^" + p.replace(/[.+?^$()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*").replace(/\{([^}]*)\}/g, (_, a) => "(?:" + a.split(",").join("|") + ")") + "$").test(f) : f === p;
        if (m) { if (neg) return false; hit = true; }
    }
    return hit;
}
function jRule18(S, emit) {
    const mt = S.get("tools/modules.tsv");
    if (!mt) return;
    const rows = mt.raw.split("\n").slice(1).map((l) => l.split("\t")).filter((c) => c.length >= 8 && c[1] === "module");
    const fuzz = S.ctxs.filter((f) => /^src\/fuzz\/.*\.c$/.test(f.path));
    const byDir = new Map();
    for (const f of S.ctxs) { const m = /^tests\/[^/]+\//.exec(f.path); if (m && /\.(?:js|mjs|c)$/.test(f.path)) { if (!byDir.has(m[0])) byDir.set(m[0], []); byDir.get(m[0]).push(f); } }
    const under = (d) => /^tests\/[^/]+\/$/.test(d) ? byDir.get(d) || [] : S.ctxs.filter((f) => f.path.startsWith(d) && /\.(?:js|mjs|c)$/.test(f.path));
    const table = [];
    for (const c of rows) {
        const mod = c[0];
        const dirs = (c[7] || "").split(/\s+/).filter(Boolean).map((d) => d.replace(/\/?$/, "/"));
        if (!dirs.length) dirs.push("tests/" + mod + "/");
        const tf = [...new Set(dirs.flatMap(under))];
        const txt = tf.map((f) => f.raw).join("\n");
        const has = {};
        for (const [k, re] of R18) has[k] = re.test(k === "mallocfail" ? tf.filter((f) => f.path.endsWith(".c")).map((f) => f.raw).join("\n") : txt);
        const kitList = (c[4] || "").split(/\s+/).filter(Boolean);
        const kits = [...new Set([...kitList.filter((k) => !/[*{!]/.test(k) && !k.endsWith("/")), ...S.paths.filter((p) => /\.[ch]$/.test(p) && kitIn(kitList, p))])].map((k) => k.replace(/^.*\//, "").replace(/\.[ch]$/, "").replace(/^dyn-/, "").replace(/-/g, "_"));
        const mrx = new RegExp("(?:^|/)fuzz_(?:" + [mod.replace(/-/g, "_"), ...kits].map(esc).join("|") + ")|modules/" + esc(mod) + "/");
        has.fuzz = fuzz.some((f) => mrx.test(f.path) || mrx.test(f.raw));
        const gaps = Object.keys(has).filter((k) => !has[k]);
        table.push([mod, tf.length, ...Object.keys(has).map((k) => (has[k] ? 1 : 0)), gaps.join(",")]);
        if (gaps.length) emit("D01", { path: dirs[0].replace(/\/$/, ""), raw: "", line: () => 0, text: () => "" }, 0, mod + " missing: " + gaps.join(","), gaps.length >= 4 ? "med" : "low");
    }
    S.r18 = { cols: ["module", "testFiles", ...R18.map((r) => r[0]), "fuzz", "gaps"], rows: table };
}
function jCtest(S, emit) {
    const run = S.get("tests/ctest/run.sh");
    if (!run) return;
    const words = (k) => { const m = new RegExp("^" + k + "=\"([^\"]*)\"", "m").exec(run.raw); return m ? m[1].split(/\s+/).filter(Boolean) : []; };
    const targets = new Set([...words("TARGETS"), ...words("SAN_TARGETS")]);
    const recipes = new Map();
    const lab = /^[ \t]*([\w-]+(?:\s*\|\s*[\w-]+)*)\)[ \t]*$/gm;
    const labs = [];
    let m;
    while ((m = lab.exec(run.raw))) labs.push({ names: m[1].split(/\s*\|\s*/), idx: m.index, body: m.index + m[0].length });
    for (let i = 0; i < labs.length; i++) {
        const body = run.raw.slice(labs[i].body, labs[i + 1] ? labs[i + 1].idx : run.raw.length);
        for (const n of labs[i].names) recipes.set(n, { body, idx: labs[i].idx });
    }
    const inTargets = new Set(), inAny = new Set();
    for (const [n, r] of recipes) for (const p of [...(r.body.match(/[\w.\/$-]+\.c\b/g) || []), ...(r.body.match(/\btest_\w+\b/g) || []).map((w) => w + ".c")]) { const b = p.split("/").pop(); inAny.add(b); if (targets.has(n)) inTargets.add(b); }
    for (const t of targets) if (!recipes.has(t) && !/^tsan-/.test(t)) emit("E02", run, run.raw.indexOf(t), "TARGETS entry " + t + " has no case recipe");
    for (const [n, r] of recipes) for (const p of r.body.match(/(?:^|\s)(tests\/[\w.\/-]+\.c)\b/g) || []) { const q = p.trim(); if (!S.get(q)) emit("E02", run, r.idx, "recipe " + n + " names missing " + q); }
    const other = S.ctxs.filter((f) => f.path !== run.path && (SH.test(f.path) || /^tools\/.*\.js$/.test(f.path))).map((f) => f.raw).join("\n");
    for (const f of S.ctxs) {
        if (!/^tests\/(?:.*\/)?test_[^/]*\.c$/.test(f.path) || !/\bint\s+main\s*\(/.test(f.raw)) continue;
        const b = f.path.split("/").pop();
        if (inTargets.has(b) || targets.has(b.replace(/\.c$/, "").replace(/_/g, "-"))) continue;
        if (inAny.has(b)) emit("E01", f, f.raw.search(/\bint\s+main\s*\(/), b + " has a run.sh recipe that is not in TARGETS");
        else if (!other.includes(b)) emit("E01", f, f.raw.search(/\bint\s+main\s*\(/), b + " is built by no run.sh recipe or other script");
    }
}
function suiteSet(S) {
    const ku = S.get("tests/known-unrun.txt");
    const skip = new Set(ku ? ku.raw.split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t")[0].trim()) : []);
    return S.ctxs.filter((f) => /^tests\/.*\/(?:test_|bb_)[^/]*\.js$/.test(f.path) && !/\/lib\//.test(f.path) && !/_child\.js$/.test(f.path)
        && !/^tests\/(?:sdl|ui|raylib|uring|agent)\//.test(f.path) && !skip.has(f.path));
}
function jSuites(S, emit) {
    const suites = suiteSet(S);
    const assertRe = /\bassert\w*\s*\(|\bthrow\b|\bexit\s*\(|\bfail\w*\s*\(|\bexpect\w*\s*\(|\bcheck\w*\s*\(|\beq\s*\(|\bok\s*\(|\bdeepEqual|\bsame\s*\(|\bt\.\w+\s*\(|\brun\w*\s*\(|\bcase\w*\s*\(|\btest\w*\s*\(|from\s+["'][^"']*\/lib\/(?:kit|assert|harness|check)/;
    for (const f of suites) if (!assertRe.test(f.raw)) emit("E03", f, 0, f.path.split("/").pop() + " has no assertion");
    for (const f of S.ctxs) {
        if (!/^tests\/.*\.(?:js|mjs)$/.test(f.path) || /^tests\/lib\/services\.js$/.test(f.path)) continue;
        const st = /\bensure(?:Redis|Postgres|Exim|Mailpit)\s*\(/.exec(f.raw);
        if (st && !/\bstop\w*\s*\(|\bteardown\w*\s*\(/.test(f.raw)) emit("E06", f, st.index, st[0].trim() + " without stopStarted()/stop");
    }
    const names = new Map(suites.map((f) => [f.path.split("/").pop(), f]));
    const refRe = /["'`/]((?:test|bb)_[\w-]+\.js)["'`]/g;
    const hit = new Set();
    for (const f of S.ctxs) {
        if (!/^tests\/.*\.(?:js|mjs)$/.test(f.path)) continue;
        let m;
        refRe.lastIndex = 0;
        while ((m = refRe.exec(f.raw))) {
            const t = names.get(m[1]);
            if (!t || t === f || hit.has(t.path) || t.raw.includes(f.path.split("/").pop())) continue;
            const ctxLine = f.text(f.line(m.index));
            if (!/\bimport\b|\bfrom\b|Worker|Exec|Spawn|spawn|exec|run\w*\(|DYNAJS|dynajs|argv|\[\s*["'][^"']*["']\s*,/.test(ctxLine)) continue;
            if (/test262|tests\.txt|suites|known-unrun|skip|allow/i.test(ctxLine)) continue;
            hit.add(t.path);
            emit("E08", t, 0, m[1] + " referenced by " + f.path + ":" + (f.line(m.index) + 1));
        }
    }
}
function jLists(S, emit) {
    const al = S.get("tests/std_os_subject.txt");
    if (al) {
        const ls = al.ls;
        al.raw.split("\n").forEach((l, i) => {
            const p = l.trim();
            if (!p || p.startsWith("#")) return;
            const f = S.get(p.split(/\s+/)[0]);
            if (!f) emit("E07", al, ls[i], p + " does not exist");
            else if (!/from\s*["'](?:std|os)["']|import\s*\(\s*["'](?:std|os)["']|globalThis\.std\b/.test(f.raw)) emit("E07", al, ls[i], p + " no longer uses std/os");
        });
    }
    const ku = S.get("tests/known-unrun.txt");
    if (ku) ku.raw.split("\n").forEach((l, i) => {
        const p = l.split("\t")[0].trim();
        if (!p || p.startsWith("#") || !/\.js$/.test(p)) return;
        if (!S.get(p) && !S.exists(p)) emit("E07", ku, ku.ls[i], p + " (known-unrun) does not exist");
    });
}
function jPython(S, emit) {
    for (const p of S.paths) if (/\.py[co]?$/.test(p) || /(?:^|\/)__pycache__(?:\/|$)/.test(p)) emit("F07b", { path: p, raw: "", line: () => 0, text: () => "" }, 0, p);
}
function jRunFromLog(S, emit) {
    const LOGSRC = /\.log\b|ledger\.tsv|summary\.txt/;
    for (const f of [...S.ctxs, ...(S.aux || [])]) {
        if (!SH.test(f.path)) continue;
        const vars = new Set();
        const lines = f.raw.split("\n");
        let off = 0;
        for (const l of lines) {
            const a = /^\s*(?:local\s+|export\s+)?([A-Za-z_]\w*)=\$\((.*)/.exec(l);
            if (a && LOGSRC.test(a[2])) vars.add(a[1]);
            const b = /(?:mapfile|readarray)\s+(?:-\w+\s+)*([A-Za-z_]\w*)\s*<\s*<\((.*)/.exec(l);
            if (b && LOGSRC.test(b[2])) vars.add(b[1]);
            if (/\brun2\.sh\b|\bmerge\w*\.sh\b/.test(l) && !/^\s*#/.test(l)) {
                const direct = /\$\([^)]*/.exec(l);
                const viaVar = [...vars].find((v) => new RegExp("\\$\\{?" + v + "\\b").test(l));
                if ((direct && LOGSRC.test(direct[0])) || viaVar) emit("H01", f, off, viaVar ? "run list $" + viaVar + " comes from a log" : "run list from a log: " + l.trim().slice(0, 120));
            }
            off += l.length + 1;
        }
    }
}
function covMapAge(head, merges) {
    const b = /\tbuilt=(\d+)/.exec(head || "");
    if (!b) return null;
    const d = new Date(+b[1] * 1000), day = d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0");
    const rows = (merges || "").split("\n").filter((l) => /^\d{4}-\d\d-\d\d\t/.test(l)), at = /\tmerges=(\d+)/.exec(head);
    return { day, n: at ? rows.length - +at[1] : rows.filter((l) => l.slice(0, 10) > day).length };
}
function jCovMap(S, emit) {
    const m = S.covmap || { map: S.get(".obj/cov/map.tsv") ? S.get(".obj/cov/map.tsv").raw : null, merges: S.get(".agent-work/merges.tsv") ? S.get(".agent-work/merges.tsv").raw : "" };
    if (!m.map) return;
    const a = covMapAge(m.map.split("\n")[0], m.merges);
    const f = { path: ".obj/cov/map.tsv", raw: "", ls: null, line: () => 0, text: () => "" };
    if (!a) emit("H02", f, 0, "map.tsv has no built= header: rebuild with tools/coverage-map.js all");
    else if (a.n > 20) emit("H02", f, 0, "built " + a.day + ", " + a.n + " merges since (limit 20): rebuild with tools/coverage-map.js all");
}
function jBrew(S, emit) {
    for (const f of S.ctxs) {
        if (!SH.test(f.path) && !/^tools\/.*\.js$/.test(f.path)) continue;
        if (f.path === "tools/sweep.js") continue;
        const st = /brew\s+services\s+(?:start|run|restart)\b/.exec(f.raw);
        if (st && !/brew\s+services\s+stop\b/.test(f.raw)) emit("F09", f, st.index, st[0]);
    }
}

function metrics(S) {
    const out = { anyObject: {}, file: null, stdHeaders: 0, stdImports: {} };
    for (const f of S.ctxs) {
        const m = /^src\/modules\/([^/]+)\/\1\.d\.ts$/.exec(f.path);
        if (m) {
            const t = stripTsComments(f.raw);
            const a = (t.match(/\bany\b/g) || []).length, o = (t.match(/\bobject\b/g) || []).length;
            if (a + o) out.anyObject[m[1]] = { any: a, object: o };
            if (m[1] === "file") {
                const d = /^[ \t]*(?:static\s+)?(?:async\s+)?(?:function\s+)?[A-Za-z_$][\w$]*\s*(?:<[^>]*>)?\s*\(/gm;
                let k, sync = 0, asyn = 0;
                while ((k = d.exec(t))) {
                    let depth = 0, i = k.index + k[0].length - 1;
                    for (; i < t.length; i++) { if (t[i] === "(") depth++; else if (t[i] === ")" && --depth === 0) break; }
                    const ret = /^\s*:\s*([^;{]*)/.exec(t.slice(i + 1, i + 200));
                    if (!ret) continue;
                    if (/^Promise\b|^AsyncIterable/.test(ret[1].trim())) asyn++; else sync++;
                }
                out.file = { sync, async: asyn };
            }
        }
        if (/^tests\/.*\.(?:js|mjs)$/.test(f.path) && /^\/\/ flags:.*--std\b/m.test(f.raw.slice(0, 400))) out.stdHeaders++;
        if (/\.(?:js|mjs)$/.test(f.path) && /\bfrom\s*["'](?:std|os)["']|import\s*\(\s*["'](?:std|os)["']\s*\)/.test(f.raw)) {
            const top = f.path.split("/")[0];
            out.stdImports[top] = (out.stdImports[top] || 0) + 1;
        }
    }
    return out;
}

const READ_EXT = /\.(?:c|h|js|mjs|cjs|ts|sh|bash)$/;
const LIST_FILES = /^(?:tests\/[^/]+\.txt|tools\/[^/]+\.(?:tsv|txt)|README\.md|build\.sh)$/;
const SKIP_DIRS = new Set([".git", ".agent-work", ".claude", "tmp", "logs", "node_modules", "corpus", "test262", "vendor", "third_party", "asan-out", "worktrees"]);
const MAX_BYTES = 1536 * 1024;

function walk(root) {
    const files = [], paths = [];
    let skippedBig = 0;
    const visit = (rel) => {
        let ents;
        try { ents = file.readDir(rel ? root + "/" + rel : root); } catch { return; }
        for (const e of ents) {
            const p = rel ? rel + "/" + e.name : e.name;
            if (e.isSymlink) continue;
            if (e.isDir) {
                if (SKIP_DIRS.has(e.name) || e.name.startsWith(".obj") || (!rel && e.name.startsWith(".") && e.name !== ".github")) continue;
                if (e.name === "__pycache__") { paths.push(p); continue; }
                if (!rel && !/^(?:src|tests|tools|examples|bench|android|docker|\.github|scripts)$/.test(e.name)) continue;
                visit(p);
            } else if (e.isFile) {
                paths.push(p);
                if (!READ_EXT.test(e.name) && !LIST_FILES.test(p)) continue;
                if (!rel && !/\.(?:sh|md|d\.ts)$/.test(e.name)) continue;
                files.push(p);
            }
        }
    };
    visit("");
    const agentSh = (rel, depth) => {
        let ents;
        try { ents = file.readDir(root + "/" + rel); } catch { return; }
        for (const e of ents) {
            if (e.isSymlink) continue;
            const p = rel + "/" + e.name;
            if (e.isFile && /\.(?:sh|bash)$/.test(e.name)) files.push(p);
            else if (e.isDir && depth > 0) agentSh(p, depth - 1);
        }
    };
    agentSh(".agent-work", 0);
    agentSh(".agent-work/manual-run", 0);
    try { for (const e of file.readDir(root + "/.agent-work")) if (e.isDir && !e.isSymlink && /^work/.test(e.name)) agentSh(".agent-work/" + e.name + "/tmp", 3); } catch { }
    const map = new Map();
    let bytes = 0;
    for (const p of files) {
        let st;
        try { st = file.stat(root + "/" + p); } catch { continue; }
        if (st.size > MAX_BYTES) { skippedBig++; continue; }
        try { const s = file.readFile(root + "/" + p); map.set(p, s); bytes += s.length; } catch { }
    }
    return { map, paths, bytes, skippedBig };
}

function walkAux(root, aw) {
    const out = new Map();
    const pre = aw.startsWith(root + "/") ? aw.slice(root.length + 1) : aw === root.replace(/\/[^/]+$/, "") ? ".." : "AW";
    const scan = (dir, rel, depth) => {
        let es;
        try { es = file.readDir(dir); } catch { return; }
        for (const e of es) {
            if (e.isDir && depth < 1 && !/^(?:work|sweep|static|tmp|logs|asan|corpus|node_modules|prettier|android|bench|\.)/.test(e.name)) scan(dir + "/" + e.name, rel + e.name + "/", depth + 1);
            else if (e.isFile && /\.sh$/.test(e.name)) { const t = readText(dir + "/" + e.name); if (t !== null && t.length < MAX_BYTES) out.set(pre + "/" + rel + e.name, t); }
        }
    };
    scan(aw, "", 0);
    return out;
}
function patchHunks(text) {
    const m = new Map(), lines = text.split("\n");
    let cur = null, rm = 0, ad = 0;
    const open = (p) => { cur = p; if (cur && !m.has(cur)) m.set(cur, { plus: [], minus: [] }); };
    for (let i = 0; i < lines.length; i++) {
        const l = lines[i];
        if (rm > 0 || ad > 0) {
            if (l[0] === "+") { if (cur) m.get(cur).plus.push(l.slice(1)); ad--; }
            else if (l[0] === "-") { if (cur) m.get(cur).minus.push(l.slice(1)); rm--; }
            else if (l[0] === " " || l === "") { rm--; ad--; }
            else if (l[0] !== "\\") { rm = ad = 0; i--; }
            continue;
        }
        if (l.startsWith("diff --git ")) { const mm = / b\/(.+)$/.exec(l); open(mm ? mm[1] : null); continue; }
        if (l.startsWith("--- ") && i + 1 < lines.length && lines[i + 1].startsWith("+++ ")) {
            const nb = /^\+\+\+ (?:b\/)?([^\t]+)/.exec(lines[i + 1]), na = /^--- (?:a\/)?([^\t]+)/.exec(l);
            open(nb && nb[1] !== "/dev/null" ? nb[1] : na ? na[1] : null);
            i++;
            continue;
        }
        const h = /^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@/.exec(l);
        if (h) { rm = h[1] === undefined ? 1 : +h[1]; ad = h[2] === undefined ? 1 : +h[2]; }
    }
    return m;
}
const R6_TRIVIAL = /^\s*(?:[{}()\[\];,]*|fi|done|esac|else|then|do|\*\/|\/\*\*?|#|\/\/|break;|return;|continue;)\s*$/;
const R6_WORD = /[A-Za-z_][\w.-]{5,}/g;
function r6Words(lines) { const w = new Set(); for (const l of lines) for (const m of l.matchAll(R6_WORD)) w.add(m[0]); return w; }
function r6GlobKept(minusLine, w, plus) {
    const key = minusLine.split("\t")[0];
    const tok = minusLine.split(/\s+/).find((t) => t.includes(w));
    if (!tok) return false;
    for (const pl of plus) {
        if (pl.split("\t")[0] !== key) continue;
        const ents = pl.split(/\s+/).filter(Boolean);
        if (ents.some((e) => /[*{]/.test(e)) && kitIn(ents, tok)) return true;
    }
    return false;
}
function mergeGuard(patchText, merged, mainText) {
    const added = new Map(), addedW = new Map();
    for (const { name, text } of merged) {
        for (const [f, h] of patchHunks(text)) {
            let a = added.get(f);
            if (!a) added.set(f, a = new Map());
            for (const l of h.plus) if (!R6_TRIVIAL.test(l)) a.set(l.trim(), name);
            let aw = addedW.get(f);
            if (!aw) addedW.set(f, aw = new Map());
            const was = r6Words(h.minus);
            for (const w of r6Words(h.plus)) if (!was.has(w)) aw.set(w, name);
        }
    }
    const out = [], seen = new Set();
    for (const [f, h] of patchHunks(patchText)) {
        const a = added.get(f);
        if (!a) continue;
        const main = mainText(f);
        if (main === null) continue;
        const mainLines = new Set(main.split("\n").map((s) => s.trim()));
        const readd = new Set(h.plus.map((s) => s.trim()));
        const keptW = r6Words(h.plus.map((s) => s.replace(/(^|\s)![^\s]+/g, "$1")));
        for (const l of h.minus) {
            const k = l.trim();
            const lw = [...r6Words([l])];
            const replaced = lw.length > 0 && h.plus.some((p) => { const pw = r6Words([p]); return lw.some((w) => pw.has(w)); });
            if (R6_TRIVIAL.test(l) || readd.has(k) || seen.has(f + "\0" + k) || (replaced && !lw.some((w) => !keptW.has(w) && addedW.get(f).has(w) && !r6GlobKept(l, w, h.plus)))) continue;
            const who = a.get(k);
            if (who && mainLines.has(k)) { seen.add(f + "\0" + k); out.push({ file: f, line: k, by: who }); }
        }
        const aw = addedW.get(f), mainW = r6Words([main]);
        const rare = (w) => /[-./]/.test(w) || main.split(w).length <= 3;
        for (const w of r6Words(h.minus)) {
            if (keptW.has(w) || !mainW.has(w) || !aw.has(w) || !rare(w) || h.minus.some((l) => l.includes(w) && r6GlobKept(l, w, h.plus)) || out.some((o) => o.file === f && o.line.includes(w))) continue;
            out.push({ file: f, line: "word " + w, by: aw.get(w) });
        }
    }
    return out;
}
function findPatches(aw, sinceMs, skip) {
    const out = [];
    const scan = (dir, depth) => {
        let es;
        try { es = file.readDir(dir); } catch { return; }
        for (const e of es) {
            const p = dir + "/" + e.name;
            if (e.isDir && depth < 3 && !/^(?:src|tests|tools|deps|corpus|node_modules|logs|asan-out|sweep|\.)/.test(e.name)) scan(p, depth + 1);
            else if (e.isFile && /\.patch$/.test(e.name) && p !== skip && mtime(p) >= sinceMs) out.push(p);
        }
    };
    scan(aw, 0);
    return out.sort();
}
function mergeTokens(name) {
    const out = [];
    let prev = null;
    for (let t of name.replace(/\([^)]*\)/g, " ").toLowerCase().split(/[\s,]+/).flatMap((x) => x.split("+"))) {
        if (!t || /^\d[\d.-]*$/.test(t)) continue;
        const rg = /^(.*?)(\d+)\.\.([a-z]*)(\d+)$/.exec(t);
        if (rg) { for (let k = +rg[2]; k <= +rg[4] && k - +rg[2] < 20; k++) out.push(rg[1] + k); prev = rg[1] + rg[4]; continue; }
        const rd = /^([a-z][\w-]*?)(\d+)-(\d+)$/.exec(t);
        if (rd && +rd[3] > +rd[2]) { for (let k = +rd[2]; k <= +rd[3] && k - +rd[2] < 20; k++) out.push(rd[1] + k); prev = rd[1] + rd[3]; continue; }
        if (prev && prev.includes("-") && !t.includes("-") && t.length <= 3) out.push(prev.replace(/-[^-]*$/, "-" + t));
        out.push(t);
        prev = t;
    }
    return out;
}
function stemMatches(stem, t) {
    return (t.length >= 3 && stem === t) || (t.length >= 3 && stem.startsWith(t + "-")) || (/\d/.test(stem) && t.endsWith("-" + stem)) || (t.length >= 3 && stem.endsWith("-" + t));
}
function mergedPatchFilter(tsv, sinceMs) {
    const rows = [], paths = [];
    const day = (d) => Date.parse(d + "T00:00:00Z");
    for (const l of (tsv || "").split("\n")) {
        const c = l.split("\t");
        if (c.length < 2 || !/^\d{4}-\d\d-\d\d$/.test(c[0]) || day(c[0]) < sinceMs - 86400000) continue;
        const ex = [...l.matchAll(/[\w./-]+\.patch\b/g)].map((m) => m[0].replace(/^\.\//, ""));
        paths.push(...ex);
        rows.push({ name: c[1], toks: mergeTokens(c[1]), hit: ex.length > 0 });
    }
    const isMerged = (p) => {
        const stem = p.replace(/^.*\//, "").replace(/\.patch$/, "").toLowerCase();
        if (paths.some((x) => p === x || p.endsWith("/" + x))) return true;
        let any = false;
        for (const r of rows) if (r.toks.some((t) => stemMatches(stem, t))) { r.hit = true; any = true; }
        return any;
    };
    isMerged.unmatched = () => rows.filter((r) => !r.hit).map((r) => r.name);
    return isMerged;
}
function patchBlocks(text) {
    const m = new Map(), lines = text.split("\n");
    let cur = null, blk = null, rm = 0, ad = 0;
    for (let i = 0; i < lines.length; i++) {
        const l = lines[i];
        if (rm > 0 || ad > 0) {
            if (l[0] === "+") { blk.nw.push(l.slice(1)); ad--; }
            else if (l[0] === "-") { blk.old.push(l.slice(1)); rm--; }
            else if (l[0] === " " || l === "") { blk.old.push(l.slice(1)); blk.nw.push(l.slice(1)); rm--; ad--; }
            else if (l[0] !== "\\") { rm = ad = 0; i--; }
            continue;
        }
        if (l.startsWith("diff --git ")) { const mm = / b\/(.+)$/.exec(l); cur = mm ? mm[1] : null; continue; }
        if (l.startsWith("--- ") && i + 1 < lines.length && lines[i + 1].startsWith("+++ ")) {
            const nb = /^\+\+\+ (?:b\/)?([^\t]+)/.exec(lines[i + 1]), na = /^--- (?:a\/)?([^\t]+)/.exec(l);
            cur = nb && nb[1] !== "/dev/null" ? nb[1] : na ? na[1] : null;
            i++;
            continue;
        }
        const h = /^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@/.exec(l);
        if (h && cur) {
            rm = h[1] === undefined ? 1 : +h[1];
            ad = h[2] === undefined ? 1 : +h[2];
            blk = { old: [], nw: [] };
            if (!m.has(cur)) m.set(cur, []);
            m.get(cur).push(blk);
        }
    }
    return m;
}
function r7ApplyText(files, text, reverse, only) {
    const out = new Map(files);
    for (const [f, blocks] of patchBlocks(text)) {
        if (only && !only.includes(f)) continue;
        let lines = out.has(f) ? out.get(f).split("\n") : [];
        for (const b of blocks) {
            const from = reverse ? b.nw : b.old, to = reverse ? b.old : b.nw;
            let at = -1;
            for (let i = 0; i + from.length <= lines.length && at < 0; i++) { let k = 0; while (k < from.length && lines[i + k] === from[k]) k++; if (k === from.length) at = i; }
            if (at < 0) return null;
            lines = lines.slice(0, at).concat(to, lines.slice(at + from.length));
        }
        out.set(f, lines.join("\n"));
    }
    return out;
}
function r7Check(cand, merged, apply) {
    if (apply(cand, null) || apply(cand, "applied")) return null;
    const files = new Set(patchHunks(cand).keys());
    for (const m of merged) {
        const mf = [...patchHunks(m.text).keys()].filter((f) => files.has(f));
        if (mf.length && apply(cand, { text: m.text, files: mf })) return { kind: "cumulative", by: m.name };
    }
    return { kind: "noapply" };
}
function r7Git(root, tmp) {
    const git = (args, cwd) => sys.Exec("git", args, { cwd, maxBuffer: 64 << 20 }).code === 0;
    return (cand, rev) => {
        try { file.removeAll(tmp); } catch {}
        file.makeDir(tmp, { recursive: true });
        new file.File(tmp + "/cand.patch").writeText(cand);
        if (!rev) return git(["apply", "--check", tmp + "/cand.patch"], root);
        if (rev === "applied") return git(["apply", "-R", "--check", tmp + "/cand.patch"], root);
        const tree = tmp + "/tree";
        file.makeDir(tree, { recursive: true });
        git(["init", "-q", "."], tree);
        for (const f of new Set([...patchHunks(cand).keys(), ...rev.files])) {
            const t = readText(root + "/" + f);
            if (t === null) continue;
            file.makeDir((tree + "/" + f).replace(/\/[^/]+$/, ""), { recursive: true });
            new file.File(tree + "/" + f).writeText(t);
        }
        new file.File(tmp + "/rev.patch").writeText(rev.text);
        if (!git(["apply", "-R", ...rev.files.map((f) => "--include=" + f), tmp + "/rev.patch"], tree)) return false;
        return git(["apply", "--check", tmp + "/cand.patch"], tree);
    };
}
function patchGuard(o, root, aw) {
    const t0 = nowMs();
    const ptext = readText(o.patch);
    if (ptext === null) throw new Error("--patch: cannot read " + o.patch);
    const sinceMs = o.since ? Date.parse(o.since + "T00:00:00Z") : Date.now() - 3 * 86400000;
    if (!Number.isFinite(sinceMs)) throw new Error("--since wants YYYY-MM-DD");
    const abs = (p) => p.startsWith("/") ? p : sys.cwd() + "/" + p;
    const self = abs(o.patch);
    const selfDir = self.replace(/\/[^/]+$/, "");
    const merged0 = mergedPatchFilter(readText(aw + "/merges.tsv"), sinceMs);
    const found = findPatches(aw, sinceMs, self).filter((p) => p.replace(/\/[^/]+$/, "") !== selfDir);
    const paths = found.filter(merged0);
    const um = merged0.unmatched();
    if (um.length) console.log("R6 note: " + um.length + " merges.tsv row(s) since --since matched no *.patch on disk (their adds are not guarded): " + um.join(" | "));
    const merged = paths.map((p) => ({ name: p.startsWith(aw + "/") ? p.slice(aw.length + 1) : p, text: readText(p) || "" }));
    const hits = mergeGuard(ptext, merged, (f) => readText(root + "/" + f));
    for (const h of hits) console.log("R6\t" + h.file + "\t" + h.line.slice(0, 160) + "\tadded by " + h.by);
    const r7tmp = (o.out ? abs(o.out) : aw + "/sweep") + "/r7";
    const r7merged = findPatches(aw, sinceMs, self).filter(merged0).map((p) => ({ name: p.startsWith(aw + "/") ? p.slice(aw.length + 1) : p, text: readText(p) || "" }));
    const r7 = r7Check(ptext, r7merged, r7Git(root, r7tmp));
    try { file.removeAll(r7tmp); } catch {}
    if (r7) console.log(r7.kind === "cumulative" ? "R7 cumulative: patch includes merged " + r7.by + " (recut it against main)" : "R7 does not apply: git apply --check fails on " + root + " and no merged patch of the last days explains it");
    console.log("sweep --patch: " + hits.length + " removed line(s) that a merged patch added; " + merged.length + " merged patch(es) (of " + found.length + " on disk, per merges.tsv) since " + new Date(sinceMs).toISOString().slice(0, 10) + "; " + Math.round(nowMs() - t0) + " ms");
    return hits.length || r7 ? 1 : 0;
}
function analyze(map, paths, o = {}) {
    const ctxs = [];
    const byPath = new Map();
    for (const [p, raw] of map) { const c = new Ctx(p, raw); ctxs.push(c); byPath.set(p, c); }
    const S = { ctxs, aux: o.aux ? [...o.aux].map(([p, raw]) => new Ctx(p, raw)) : [], covmap: o.covmap, paths: paths || [...map.keys()], get: (p) => byPath.get(p), exists: o.exists || (() => false) };
    const findings = [];
    const sevOf = new Map([...RULES, ...JOINS].map((r) => [r.id, r]));
    const want = (r) => !o.only || o.only.has(r.id) || o.only.has(r.cat);
    const emit = (id, f, idx, note, sev) => {
        const r = sevOf.get(id);
        if (!want(r)) return;
        const ln = typeof idx === "number" && f.ls ? f.line(idx) : 0;
        findings.push({ rule: id, sev: sev || r.sev, file: f.path, line: ln + 1, snippet: (note || f.text(ln)).replace(/\s+/g, " ").trim().slice(0, 200), hint: r.hint });
    };
    const lineRules = RULES.filter(want);
    const tm = o.timing || null;
    for (const f of ctxs) {
        for (const r of lineRules) {
            const t0 = tm ? nowMs() : 0;
            ruleOnFile(r, f);
            if (tm) tm[r.id] = (tm[r.id] || 0) + nowMs() - t0;
        }
    }
    function ruleOnFile(r, f) {
        {
            if (!r.glob.test(f.path) || (r.except && r.except.test(f.path))) return;
            if (f.path.startsWith(".agent-work/") !== !!r.agent) return;
            if (r.key && !(Array.isArray(r.key) ? r.key.some((k) => f.raw.includes(k)) : f.raw.includes(r.key))) return;
            if (r.fn) {
                for (const fn of f.fns) {
                    const body = f.code.slice(fn.open, fn.close + 1);
                    for (const h of r.fn(fn, body, f)) { const ln = f.line(fn.open + h.off); emit(r.id, f, fn.open + h.off, h.note ? f.text(ln).trim() + "  [" + h.note + "]" : null); }
                }
                return;
            }
            const text = r.on === "raw" ? f.raw : f.code;
            r.re.lastIndex = 0;
            let m;
            while ((m = r.re.exec(text))) {
                if (m[0] === "") { r.re.lastIndex++; continue; }
                if (r.when && !r.when(m, f)) continue;
                if (r.unless) {
                    const u = r.unless;
                    const re = typeof u.re === "function" ? u.re(m) : u.re;
                    const ln = f.line(m.index);
                    let hay;
                    if (u.scope === "fn") { const fn = fnAt(f.fns, m.index); hay = fn ? f.code.slice(fn.open, fn.close + 1) : text; }
                    else if (u.scope === "file") hay = text;
                    else hay = linesRange(text, f.ls, ln - (u.before || 0), ln + (u.after || 0));
                    if (u.before && !u.after) hay = hay.slice(0, hay.length - lineText(text, f.ls, ln).length) + lineText(text, f.ls, ln).slice(0, m.index - f.ls[ln]);
                    if (re.test(hay)) continue;
                }
                emit(r.id, f, m.index, null);
            }
        }
    }
    for (const j of JOINS) {
        if (!j.run) continue;
        const ids = [j.id, ...JOINS.filter((x) => x.via === j.id).map((x) => x.id)];
        if (o.only && !ids.some((id) => want(sevOf.get(id)))) continue;
        const t0 = tm ? nowMs() : 0;
        j.run(S, emit);
        if (tm) tm[j.id] = (tm[j.id] || 0) + nowMs() - t0;
    }
    return { findings, S };
}

const norm = (s) => s.replace(/\s+/g, " ").replace(/\b\d+\b/g, "0").trim();
const keyText = (rule, snippet) => rule === "D01" ? snippet.replace(/ missing: .*$/, " rule18 gaps") : norm(snippet);
function keyed(findings) {
    const seen = new Map();
    return findings.map((f) => { const k = f.rule + "\t" + f.file + "\t" + keyText(f.rule, f.snippet); const n = (seen.get(k) || 0) + 1; seen.set(k, n); return k + "\t#" + n; });
}

function selftest() {
    let fails = 0, checks = 0;
    const VPATH = { A: "src/modules/z/z-a.c", C: "src/modules/z/z.d.ts", E: "tests/z/test_z.js", F: "tools/z.sh", G: "tools/z.js" };
    const vpathFor = (r) => r.vpath || (r.id === "F06" || r.id === "F07" ? "tools/z.sh" : r.id === "E05" ? "tests/z/test_z.js" : VPATH[r.cat]);
    {
        checks++;
        const { findings } = analyze(new Map([[".agent-work/work9/tmp/z.sh", "#!/bin/bash\nsed -i 's/a/b/' f\nhead -n -2 f\nrsync -a a b\n"], [".agent-work/work9/tmp/export/tools/z.sh", "rsync -a a b\n"]]), null, { only: new Set(["F01", "F04", "F05", "F20"]) });
        const got = findings.map((x) => x.rule + "@" + x.file).sort().join(",");
        if (got !== "F20@.agent-work/work9/tmp/z.sh") { fails++; console.log("FAIL agent-script scope: only F20 may see .agent-work scripts, and not copied repo trees; got " + (got || "none")); }
    }
    for (const r of RULES) {
        if (!r.pos || !r.pos.length || !r.neg || !r.neg.length) { console.log("FAIL " + r.id + " " + r.name + ": needs pos and neg fixtures"); fails++; continue; }
        for (const [kind, list] of [["pos", r.pos], ["neg", r.neg]]) for (const snip of list) {
            checks++;
            const vp = vpathFor(r);
            const { findings } = analyze(new Map([[vp, snip]]), null, { only: new Set([r.id]) });
            const n = findings.filter((x) => x.rule === r.id).length;
            const ok = kind === "pos" ? n > 0 : n === 0;
            if (!ok) { fails++; console.log("FAIL " + r.id + " " + r.name + " " + kind + " (" + n + " hits): " + JSON.stringify(snip).slice(0, 140)); }
        }
    }
    for (const j of JOINS) {
        if (!j.run) continue;
        if (!j.pos || !j.neg) { console.log("FAIL " + j.id + ": needs pos and neg fixtures"); fails++; continue; }
        for (const [kind, list] of [["pos", j.pos], ["neg", j.neg]]) for (const fx of list) {
            checks++;
            const map = new Map(Object.entries(fx));
            const { findings } = analyze(map, [...map.keys()], { only: new Set([j.id]) });
            const n = findings.filter((x) => x.rule === j.id).length;
            const ok = kind === "pos" ? n > 0 : n === 0;
            if (!ok) { fails++; console.log("FAIL " + j.id + " " + j.name + " " + kind + " (" + n + " hits): " + findings.map((x) => x.snippet).join(" | ").slice(0, 200)); }
        }
    }
    const viaFx = [
        ["C02", { "src/modules/z/z.d.ts": "declare module \"dyna:z\" {\n    function a(x: number): number;\n    function ghost(): void;\n}\n", "src/modules/z/z-m.c": "JS_CFUNC_DEF(\"a\", 1, za),\n" }, 1],
        ["C02", { "src/modules/z/z.d.ts": "declare module \"dyna:z\" {\n    function a(x: number): number;\n}\n", "src/modules/z/z-m.c": "JS_CFUNC_DEF(\"a\", 1, za),\n" }, 0],
        ["C03", { "src/modules/z/z.d.ts": "declare module \"dyna:z\" {\n    function a(x: number, y: string): number;\n}\n", "src/modules/z/z-m.c": "JS_CFUNC_DEF(\"a\", 0, za),\n" }, 1],
        ["C03", { "src/modules/z/z.d.ts": "declare module \"dyna:z\" {\n    function a(x: number, y?: Map<string, number>): number;\n}\n", "src/modules/z/z-m.c": "JS_CFUNC_DEF(\"a\", 2, za),\n" }, 0],
        ["E02", { "tests/ctest/run.sh": "TARGETS=\"test-a test-b\"\ncase \"$1\" in\n  test-a)\n    cc tests/x/test_gone.c ;;\nesac\n" }, 2],
        ["E02", { "tests/ctest/run.sh": "TARGETS=\"test-a\"\ncase \"$1\" in\n  test-a)\n    cc tests/x/test_a.c ;;\nesac\n", "tests/x/test_a.c": "int main(void){return 0;}\n" }, 0],
        ["E06", { "tests/z/test_z.js": "import { ensureRedis } from '../lib/services.js';\nawait ensureRedis();\nassert(1);\n", "tests/known-unrun.txt": "" }, 1],
        ["E06", { "tests/z/test_z.js": "import { ensureRedis, stopStarted } from '../lib/services.js';\nawait ensureRedis();\nassert(1);\nawait stopStarted();\n", "tests/known-unrun.txt": "" }, 0],
        ["E08", { "tests/z/test_z.js": "assert(1);\nconst w = new Worker('./test_z_helper.js');\n", "tests/z/test_z_helper.js": "postMessage(1);\n", "tests/known-unrun.txt": "" }, 1],
        ["E08", { "tests/z/test_z.js": "assert(1);\nconst w = new Worker('./test_z_child.js');\n", "tests/z/test_z_child.js": "postMessage(1);\n", "tests/known-unrun.txt": "" }, 0],
    ];
    for (const [id, fx, want] of viaFx) {
        checks++;
        const map = new Map(Object.entries(fx));
        const { findings } = analyze(map, [...map.keys()], { only: new Set([id]) });
        const n = findings.filter((x) => x.rule === id).length;
        if (want ? n !== want : n !== 0) { fails++; console.log("FAIL " + id + " via fixture: want " + want + " got " + n + ": " + findings.map((x) => x.snippet).join(" | ").slice(0, 200)); }
    }
    const iq = "## QUEUE (in place)\n### NEXT a 2026-11-20\n## A. Old\n## F18 OPEN 2026-11-15 old item\n## MERGE 2026-11-28: appended\nUNCONFIRMED: 2026-11-27 note\n";
    const got = issuesAfterQueue(iq, 2).map((x) => x.line).join(",");
    checks++;
    if (got !== "5,6") { fails++; console.log("FAIL R5 issuesAfterQueue: want lines 5,6 got " + got); }
    checks++;
    if (issuesAfterQueue("## QUEUE\n### NEXT x 2026-11-28\n## A. Old\n", 2).length) { fails++; console.log("FAIL R5 issuesAfterQueue flagged an in-queue item"); }
    {
        const mp = "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1,2 +1,3 @@\n x\n+run_ctest test-native-interrupt-boundary\n+fi\n";
        const mainT = { "tests/run.sh": "x\nrun_ctest test-native-interrupt-boundary\nfi\n" };
        const mt = (f) => mainT[f] ?? null;
        const mg = [{ name: "m.patch", text: mp }];
        const UBM = "x\nexport UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\"\necho UBSAN_OPTIONS halt_on_error\n";
        const UBP = [{ name: "ubsan-halt.patch", text: "diff --git a/tests/ctest/run.sh b/tests/ctest/run.sh\n--- a/tests/ctest/run.sh\n+++ b/tests/ctest/run.sh\n@@ -1,2 +1,2 @@\n x\n-export UBSAN_OPTIONS=\"halt_on_error=0:print_stacktrace=1\"\n+export UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\"\n" }];
        const GR = "structures\tlib\tsrc/core/ds/ds-aaaaa.c src/core/ds/ds-bitset.c\tStructures", GM = GR + "\n";
        const G = (plus) => "diff --git a/tools/modules.tsv b/tools/modules.tsv\n--- a/tools/modules.tsv\n+++ b/tools/modules.tsv\n@@ -1 +1 @@\n-" + GR + "\n" + plus.split("\n").map((s) => "+" + s).join("\n") + "\n";
        const GMP = [{ name: "g.patch", text: "diff --git a/tools/modules.tsv b/tools/modules.tsv\n--- a/tools/modules.tsv\n+++ b/tools/modules.tsv\n@@ -1 +1 @@\n-structures\tlib\tsrc/core/ds/ds-aaaaa.c\tStructures\n+" + GR + "\n" }];
        const cases = [
            ["drops merged line", "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1,3 +1,2 @@\n x\n-run_ctest test-native-interrupt-boundary\n-fi\n+y\n", mt, 1],
            ["moves merged line", "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1,3 +1,3 @@\n-run_ctest test-native-interrupt-boundary\n x\n+run_ctest test-native-interrupt-boundary\n", mt, 0],
            ["line no longer in main", "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1,3 +1,2 @@\n-run_ctest test-native-interrupt-boundary\n", () => "x\n", 0],
            ["drops a merged word from a changed line", "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1 +1 @@\n-T=\"test-aaaaa test-native-interrupt-boundary test-bbbbb\"\n+T=\"test-aaaaa test-bbbbb test-ccccc\"\n", () => "T=\"test-aaaaa test-native-interrupt-boundary test-bbbbb\"\n", 1, [{ name: "w.patch", text: "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1 +1 @@\n-T=\"test-aaaaa\"\n+T=\"test-aaaaa test-native-interrupt-boundary\"\n" }]],
            ["keeps the merged word", "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1 +1 @@\n-T=\"test-aaaaa test-native-interrupt-boundary\"\n+T=\"test-native-interrupt-boundary test-ccccc\"\n", () => "T=\"test-aaaaa test-native-interrupt-boundary\"\n", 0, [{ name: "w.patch", text: "diff --git a/tests/run.sh b/tests/run.sh\n--- a/tests/run.sh\n+++ b/tests/run.sh\n@@ -1 +1 @@\n-T=\"test-aaaaa\"\n+T=\"test-aaaaa test-native-interrupt-boundary\"\n" }]],
            ["common identifier is not a lost word", "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-int f(JSContext* ctx);\n+int f(void);\n", () => "int f(JSContext* ctx);\nint g(JSContext* c);\nJSContext b;\nJSContext c;\n", 0, [{ name: "c.patch", text: "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-int g(void);\n+int g(JSContext* c);\n" }]],
            ["pure deletion of a merged line whose words survive elsewhere", "diff --git a/tests/ctest/run.sh b/tests/ctest/run.sh\n--- a/tests/ctest/run.sh\n+++ b/tests/ctest/run.sh\n@@ -1,3 +1,2 @@\n x\n-export UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\"\n echo UBSAN_OPTIONS halt_on_error\n", () => UBM, 1, UBP],
            ["merged line replaced by one sharing its words", "diff --git a/tests/ctest/run.sh b/tests/ctest/run.sh\n--- a/tests/ctest/run.sh\n+++ b/tests/ctest/run.sh\n@@ -1,3 +1,3 @@\n x\n-export UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\"\n+export UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=0\"\n echo UBSAN_OPTIONS halt_on_error\n", () => UBM, 0, UBP],
            ["plain unified diff (no diff --git) dropping a merged line", "--- a/tests/ctest/run.sh\n+++ b/tests/ctest/run.sh\n@@ -1,3 +1,2 @@\n x\n-export UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\"\n echo UBSAN_OPTIONS halt_on_error\n--- a/tests/other.js\n+++ b/tests/other.js\n@@ -1 +1,2 @@\n-- a/not-a-header\n+--- a/not-a-header\n+x\n", () => UBM, 1, [{ name: "plain.patch", text: "--- a/tests/ctest/run.sh\n+++ b/tests/ctest/run.sh\n@@ -1 +1,2 @@\n x\n+export UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\"\n" }]],
            ["word still matched by a glob in the same row", G("structures\tlib\tsrc/core/ds/*.c !src/core/ds/ds-zzzzz.c\tStructures"), () => GM, 0, GMP],
            ["word excluded by the same row glob", G("structures\tlib\tsrc/core/ds/*.c !src/core/ds/ds-bitset.c\tStructures"), () => GM, 1, GMP],
            ["glob in another row does not keep it", G("structures\tlib\tsrc/core/ds/ds-aaaaa.c\tStructures\nother\tlib\tsrc/core/ds/*.c\tOther"), () => GM, 1, GMP],
            ["other file", "diff --git a/tests/b.sh b/tests/b.sh\n--- a/tests/b.sh\n+++ b/tests/b.sh\n@@ -1 +1 @@\n-run_ctest test-native-interrupt-boundary\n", mt, 0],
        ];
        for (const [name, p, f, want, mgx] of cases) {
            checks++;
            const n = mergeGuard(p, mgx || mg, f).length;
            if (n !== want) { fails++; console.log("FAIL R6 " + name + ": want " + want + " got " + n); }
        }
    }
    {
        const base = new Map([["a.c", "x\ny\nz"], ["b.c", "q"]]);
        const MERGED = { name: "m.patch", text: "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1,3 +1,3 @@\n x\n-y\n+Y\n z\n" };
        const main = r7ApplyText(base, MERGED.text, false);
        const OTHER = { name: "o.patch", text: "diff --git a/b.c b/b.c\n--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-q\n+Q\n" };
        const js = (cand, rev) => { if (rev === "applied") return r7ApplyText(main, cand, true) !== null; const t = rev ? r7ApplyText(main, rev.text, true, rev.files) : main; return t !== null && r7ApplyText(t, cand, false) !== null; };
        const cases = [
            ["already applied to the tree (a work dir that cut it)", MERGED.text + "diff --git a/b.c b/b.c\n--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-Q\n+q\n", null],
            ["applies on main", "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1,3 +1,4 @@\n x\n Y\n z\n+w\n", null],
            ["cut over the merged patch", "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1,3 +1,4 @@\n x\n-y\n+Y\n z\n+w\n", "cumulative m.patch"],
            ["stale context no merged patch explains", "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1,2 +1,2 @@\n x\n-k\n+K\n", "noapply"],
            ["only the shared files of a merged patch are reversed", "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n@@ -1,3 +1,4 @@\n x\n-y\n+Y\n z\n+v\n", "cumulative m2.patch", [{ name: "m2.patch", text: MERGED.text + "diff --git a/c.c b/c.c\n--- a/c.c\n+++ b/c.c\n@@ -1 +1 @@\n-old\n+gone-from-main\n" }, MERGED]],
            ["a merged patch on other files is no excuse", "diff --git a/b.c b/b.c\n--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-zz\n+ZZ\n", "noapply"],
        ];
        for (const [name, cand, want, list] of cases) {
            checks++;
            const r = r7Check(cand, list || [OTHER, MERGED], js);
            const got = r === null ? null : r.kind === "cumulative" ? "cumulative " + r.by : r.kind;
            if (got !== want) { fails++; console.log("FAIL R7 " + name + ": want " + want + " got " + got); }
        }
        checks++;
        if (main === null || main.get("a.c") !== "x\nY\nz") { fails++; console.log("FAIL R7 r7ApplyText fixture setup"); }
    }
    {
        const tsv = "date\tpatch\tresult\tnotes\n2026-11-28\tsweep-top20 + sweep v2 (52 rules)\tgreen\tx\n2026-11-28\tpg-copy 1-3\tgreen\twork-pg/tmp/zz-explicit.patch\n2026-11-28\tT262-STAGING-3 strict-only\tgreen\tx\n2026-11-01\told\tgreen\tx\n2026-11-28\tstd-b0\tgreen\tx\n2026-11-28\tfz5l PG-LIVE-INFRA\tgreen\tx\n2026-11-28\td49-corenet-a1+a2\tgreen\tx\n2026-11-28\tcorenet-b1..b3 + st1q\tgreen\tx\n2026-11-28\tnever-on-disk\tgreen\tx\n2026-11-28\tfz5n1-3 httpk + cookiejar\tgreen\tx\n";
        const isM = mergedPatchFilter(tsv, Date.parse("2026-11-26T00:00:00Z"));
        const cases = [["/a/work-static/tmp/top20.patch", true], ["/a/work-search/tmp/sync-20261128-000116.patch", false], ["/a/work-pg/tmp/pg-copy-2-module.patch", true],
            ["/a/work-x/old.patch", false], ["/a/work-x/v2.patch", false], ["/a/work-x/sweep.patch", true], ["/a/work-x/rules.patch", false], ["/a/w/b0.patch", true],
            ["/a/w/fz5l-pg-live-infra.patch", true], ["/a/w/d49-corenet-a2.patch", true], ["/a/w/d49-corenet-b2.patch", true], ["/a/w/st1q.patch", true], ["/a/w/pg-copy-1-wire.patch", true], ["/a/w/copy.patch", false], ["/a/w/fz5n2-cookiejar-rule18.patch", true], ["/a/work-pg/tmp/zz-explicit.patch", true], ["/a/w/zz-explicit.patch", false], ["/a/w/t262-staging-3.patch", true]];
        for (const [p, want] of cases) { checks++; if (isM(p) !== want) { fails++; console.log("FAIL R6 mergedPatchFilter " + p + ": want " + want); } }
        checks++;
        if (isM.unmatched().join(",") !== "never-on-disk") { fails++; console.log("FAIL R6 unmatched rows: " + isM.unmatched().join(",")); }
        for (const av of [["dynajs", "tools/sweep.js", "--only", "A22", "--out", "d"], ["dynajs", "--std", "tools/sweep.js", "--only", "A22", "--out", "d"], ["/x/dynajs", "--std", "--stack-size", "8", "/r/tools/sweep.js", "--only", "A22,B", "--out", "d"]]) {
            checks++;
            let po = null;
            try { po = parseArgs(scriptArgs(av)); } catch (e) { po = { err: String(e) }; }
            if (!po.only || !po.only.has("A22") || po.out !== "d") { fails++; console.log("FAIL argv " + JSON.stringify(av) + " -> " + JSON.stringify(po && (po.err || { only: po.only && [...po.only], out: po.out }))); }
        }
        {
            const ks = keyed([{ rule: "D01", file: "tests/z", snippet: "z missing: interrupt,fuzz" }, { rule: "D01", file: "tests/y", snippet: "y missing: fuzz" }]);
            const ks2 = keyed([{ rule: "D01", file: "tests/z", snippet: "z missing: fuzz" }]);
            checks++;
            if (ks[0] !== ks2[0] || ks[0] === ks[1]) { fails++; console.log("FAIL D01 must be keyed by module, not by its missing set: " + JSON.stringify([ks, ks2])); }
            const um = updateBaseline("D01\ttests/z\tz missing: interrupt,fuzz\t#1\nD01\ttests/q\tq missing: gc\t#1\n", ks2);
            checks++;
            if (um.text !== ks2[0] + "\n" || um.dropped !== 1 || um.over !== 0) { fails++; console.log("FAIL old D01 baseline rows must migrate to the module key: " + JSON.stringify(um)); }
        }
        const bt = "A\tf\ts\t#1\tjustified: unreachable, callers guard it\nB\tf\ts\t#1\nC\tf\ts\t#1\nD\tf\ts\t#1\tnote: not a justification\n";
        const pb = parseBaseline(bt);
        const want = "A\tf\ts\t#1\tjustified: unreachable, callers guard it\nB\tf\ts\t#1\n";
        const u1 = updateBaseline(bt, ["B\tf\ts\t#1", "E\tf\ts\t#1"]);
        checks++;
        if (u1.text !== want || u1.dropped !== 2 || u1.over !== 1 || pb.set.size !== 4) { fails++; console.log("FAIL baseline update: fixed rows must drop while a new finding is present, a justified row stays, a new finding is never added: " + JSON.stringify(u1)); }
        const u2 = updateBaseline(u1.text, ["B\tf\ts\t#1", "E\tf\ts\t#1"]);
        checks++;
        if (u2.text !== want || u2.dropped !== 0) { fails++; console.log("FAIL baseline update is not idempotent: " + JSON.stringify(u2)); }
        checks++;
        if (updateBaseline("B\tf\ts\t#1\n", []).text !== "") { fails++; console.log("FAIL baseline update keeps a fixed row when no finding remains"); }
    }
    {
        const sig = (fs, id) => fs.filter((f) => f.rule === id).map((f) => f.file + ":" + f.line + ":" + f.snippet).sort().join("\n");
        let bad = 0;
        const cases = [...RULES.flatMap((r) => (r.pos || []).map((s) => [r, new Map([[vpathFor(r), s]])])), ...JOINS.flatMap((j) => (j.pos || []).map((o) => [j, new Map(Object.entries(o))]))];
        for (const [r, m] of cases) {
            const full = sig(analyze(m, null, {}).findings, r.id);
            for (const only of [new Set([r.id]), new Set([r.cat])]) {
                checks++;
                const got = sig(analyze(m, null, { only }).findings, r.id);
                if (got !== full) { fails++; bad++; if (bad < 4) console.log("FAIL --only " + [...only] + " differs from the full run on " + r.id + "'s fixture: full " + JSON.stringify(full) + " only " + JSON.stringify(got)); }
            }
        }
    }
    const nrules = RULES.length + JOINS.length;
    const covered = new Set([...RULES.map((r) => r.id), ...JOINS.filter((j) => j.run).map((j) => j.id), ...viaFx.map((v) => v[0])]);
    const uncovered = [...RULES, ...JOINS].filter((r) => !covered.has(r.id)).map((r) => r.id);
    if (uncovered.length) { fails++; console.log("FAIL rules without fixtures: " + uncovered.join(",")); }
    console.log("sweep selftest: " + nrules + " rules, " + checks + " fixture checks, " + fails + " failure(s), " + Math.round(nowMs()) + " ms");
    return fails;
}

function readText(p) { try { return file.readFile(p); } catch { return null; } }
function readHead(p) { try { const f = new file.FileReader(p); try { const l = f.readLine(); return l === null ? null : l; } finally { f.close(); } } catch { return null; } }
function exec(cmd, args) {
    try { const r = sys.Exec(cmd, args, { timeout: 5000 }); return { code: r.code, out: String(r.stdout || "") }; } catch (e) { return { code: -1, out: "" }; }
}
function mtime(p) { try { return file.stat(p).mtimeMs || 0; } catch { return 0; } }

function status(root, aw) {
    const L = [];
    const issues = readText(aw + "/ISSUES.md");
    if (issues) {
        const q = issues.indexOf("## QUEUE");
        const sec = q < 0 ? "" : issues.slice(q, issues.indexOf("\n## ", q + 5) > 0 ? issues.indexOf("\n## ", q + 5) : issues.length);
        const heads = sec.split("\n").filter((l) => l.startsWith("### ")).slice(0, 15);
        L.push("QUEUE (first " + heads.length + " headings, ISSUES.md):");
        for (const h of heads) L.push("  " + h.slice(4, 150));
    } else L.push("QUEUE: ISSUES.md not found under " + aw);
    if (issues) {
        const ap = issuesAfterQueue(issues, 2);
        L.push("ISSUES appended after QUEUE (recent; belong inside it): " + (ap.length ? ap.length + " (" + ap.slice(0, 3).map((x) => "L" + x.line + " " + x.text.slice(0, 50)).join("; ") + ")" : "none"));
    }
    const merges = readText(aw + "/merges.tsv");
    if (merges) { const rows = merges.trim().split("\n").filter((l) => l && !/^date\t/.test(l)); L.push("last merges:"); for (const r of rows.slice(-5)) L.push("  " + r.slice(0, 150)); }
    const bugs = readText(aw + "/bugs.tsv");
    if (bugs) {
        const c = {};
        for (const r of bugs.trim().split("\n").slice(1)) { const k = (r.split("\t")[1] || "?").trim(); c[k] = (c[k] || 0) + 1; }
        L.push("bugs.tsv by found-by: " + Object.entries(c).sort((a, b) => b[1] - a[1]).map(([k, v]) => k + " " + v).join(", "));
    }
    const failed = [];
    for (const lp of [aw + "/manual-run/ledger.tsv", root + "/logs/run2/ledger.tsv"]) {
        const led = readText(lp);
        if (!led) continue;
        const last = new Map();
        for (const r of led.split("\n")) { const c = r.split("\t"); if (c.length >= 2 && c[0] && c[0] !== "suite") last.set(c[0], c[1]); }
        for (const [s, rc] of last) if (rc !== "0") failed.push(s + " rc=" + rc);
        L.push("ledger " + lp.replace(root + "/", "") + ": " + last.size + " suites, last row failed: " + (failed.length ? failed.slice(0, 10).join(", ") + (failed.length > 10 ? " +" + (failed.length - 10) : "") : "none"));
        failed.length = 0;
    }
    const mm = mtime(aw + "/merges.tsv");
    const pats = [];
    const scan = (d, depth) => {
        let es;
        try { es = file.readDir(d); } catch { return; }
        for (const e of es) {
            const p = d + "/" + e.name;
            if (e.isDir && depth < 2 && !/^(?:\.git|\.obj|src|tests|tools|logs|bench|examples|asan-out|corpus|node_modules)/.test(e.name)) scan(p, depth + 1);
            else if (e.isFile && /\.patch$/.test(e.name)) { const t = mtime(p); if (t > mm) pats.push(p.replace(aw + "/", "")); }
        }
    };
    if (mm) scan(aw, 0);
    L.push("patches newer than last merge: " + (pats.length ? pats.slice(0, 8).join(" ") + (pats.length > 8 ? " +" + (pats.length - 8) : "") : "none"));
    const gs = exec("git", ["-C", root, "status", "--porcelain"]);
    if (gs.code === 0) {
        const c = {};
        for (const l of gs.out.split("\n")) if (l) { const k = l.slice(0, 2).trim() || "?"; c[k] = (c[k] || 0) + 1; }
        L.push("git status: " + (Object.keys(c).length ? Object.entries(c).map(([k, v]) => k + " " + v).join(", ") : "clean"));
    }
    {
        const hits = [];
        let es = [];
        try { es = file.readDir(aw); } catch {}
        for (const e of es) {
            if (!e.isDir || !/^work/.test(e.name)) continue;
            const d = aw + "/" + e.name;
            const g = exec("git", ["-C", d, "grep", "-l", "-E", "^(<<<<<<< |=======$|>>>>>>> )", "--", "."]);
            const u = exec("git", ["-C", d, "diff", "--name-only", "--diff-filter=U"]);
            const files = [...new Set([...(g.code === 0 ? g.out.split("\n") : []), ...(u.code === 0 ? u.out.split("\n") : [])].filter(Boolean))];
            if (files.length) hits.push(e.name + ": " + files.slice(0, 4).join(" ") + (files.length > 4 ? " +" + (files.length - 4) : ""));
        }
        L.push("conflict markers in .agent-work/work*: " + (hits.length ? hits.join("; ") : "none"));
    }
    const lc = exec("launchctl", ["list"]);
    if (lc.code === 0) { const on = lc.out.split("\n").filter((l) => /homebrew\.mxcl\./.test(l) && /^\d/.test(l)).map((l) => l.replace(/.*homebrew\.mxcl\./, "")); L.push("brew services running: " + (on.length ? on.join(", ") : "none")); }
    const pg = exec("pgrep", ["-fl", "redis-server|postgres|sshd -D|/sshd |sleep [0-9]"]);
    const procs = pg.out.split("\n").filter((l) => l && !/pgrep/.test(l));
    L.push("stray processes: " + (procs.length ? procs.length + " (" + procs.slice(0, 4).map((l) => l.slice(0, 60)).join("; ") + ")" : "none"));
    const cnt = (p) => { const t = readText(p); return t === null ? "missing" : t.split("\n").filter((l) => l && !l.startsWith("#")).length; };
    L.push("defects baseline rows: tree " + cnt(root + "/tools/defects-baseline.tsv") + ", ref " + cnt(aw + "/defects-baseline.ref.tsv"));
    let logs = [];
    try { logs = file.readDir(aw + "/static").filter((e) => e.isFile && /\.log$/.test(e.name)).map((e) => e.name); } catch { }
    const lastLine = (p) => { const t = readText(p) || ""; const ls = t.trim().split("\n"); return ls[ls.length - 1] || "(empty)"; };
    const pick = logs.filter((n) => /^(?:static-checks|codegraph-report|codegraph-selfcheck|defects-scan|dod_scan|no-comments|hardening)/.test(n));
    const ageH = (p) => Math.round((Date.now() - mtime(p)) / 3600000);
    for (const n of pick) L.push("  " + n.replace(/\.log$/, "") + " (" + ageH(aw + "/static/" + n) + "h): " + lastLine(aw + "/static/" + n).slice(0, 110));
    return L;
}

function baseKey(c) { return [c[0], c[1], c[0] === "D01" ? keyText("D01", c[2] || "") : c[2], c[3]].join("\t"); }
function parseBaseline(text) {
    const why = new Map();
    const set = new Set(text.split("\n").filter(Boolean).map((l) => { const c = l.split("\t"); const k = baseKey(c); if (c.length > 4) why.set(k, c.slice(4).join(" ")); return k; }));
    return { set, why };
}
function updateBaseline(text, keys) {
    const cur = new Set(keys), base = new Set(), kept = [];
    let dropped = 0;
    for (const l of text.split("\n").filter(Boolean)) {
        const c = l.split("\t"), k = baseKey(c);
        base.add(k);
        if (cur.has(k) || /^justified: \S/.test(c.slice(4).join(" "))) kept.push(c.length > 4 ? k + "\t" + c.slice(4).join("\t") : k); else dropped++;
    }
    const over = keys.filter((k) => !base.has(k)).length;
    return { text: kept.length ? kept.join("\n") + "\n" : "", kept: kept.length, dropped, over };
}
function writeOutputs(o, out, findings, S, res) {
    file.makeDir(out + "/by-rule", { recursive: true });
    const all = [...RULES, ...JOINS];
    const full = !o.only && !o.files;
    const keys = keyed(findings);
    const prevKeys = new Set((readText(out + "/last-keys.tsv") || "").split("\n").filter(Boolean).map((l) => baseKey(l.split("\t"))));
    const prevCounts = new Map((readText(out + "/last-counts.tsv") || "").split("\n").filter(Boolean).map((l) => { const [k, v] = l.split("\t"); return [k, +v]; }));
    const isNew = keys.map((k) => prevKeys.size > 0 && !prevKeys.has(k));
    const row = (f) => [f.rule, f.sev, f.file, f.line, f.snippet.replace(/\t/g, " "), f.hint].join("\t");
    const hdr = "rule\tsev\tfile\tline\tsnippet\thint\n";
    file.writeFile(out + "/findings.tsv", hdr + findings.map(row).join("\n") + "\n");
    const delta = findings.filter((_, i) => isNew[i]);
    file.writeFile(out + "/delta.tsv", hdr + delta.map(row).join("\n") + (delta.length ? "\n" : ""));
    const counts = new Map(all.map((r) => [r.id, 0]));
    for (const f of findings) counts.set(f.rule, (counts.get(f.rule) || 0) + 1);
    for (const r of all) file.writeFile(out + "/by-rule/" + r.id + ".tsv", hdr + findings.filter((f) => f.rule === r.id).map(row).join("\n") + "\n");
    if (S.r18) file.writeFile(out + "/rule18.tsv", S.r18.cols.join("\t") + "\n" + S.r18.rows.map((r) => r.join("\t")).join("\n") + "\n");
    const blPath = out + "/baseline.tsv";
    const bl = readText(blPath);
    let blNote = "none";
    if (bl !== null) {
        const { set: b, why } = parseBaseline(bl);
        const over = keys.filter((k) => !b.has(k)).length, gone = [...b].filter((k) => !keys.includes(k)).length;
        const bad = [...why.values()].filter((v) => !/^justified: \S.{9,}/.test(v)).length;
        blNote = b.size + " rows (" + why.size + " justified" + (bad ? ", " + bad + " with a 5th column that is not 'justified: <reason>'" : "") + "), " + over + " findings not in it, " + gone + " rows fixed";
        if (o.updateBaseline && full) { const u = updateBaseline(bl, keys); file.writeFile(blPath, u.text); blNote += " (shrunk to " + u.kept + " rows, " + u.dropped + " dropped" + (u.over ? "; " + u.over + " findings not in it stay out, shrink-only" : "") + ")"; }
    } else if (o.updateBaseline && full) { file.writeFile(blPath, keys.join("\n") + "\n"); blNote = "created with " + keys.length + " rows"; }
    const st = res.status;
    file.writeFile(out + "/status.txt", st.join("\n") + "\n");
    const ruleRows = all.map((r) => ({ id: r.id, cat: r.cat, sev: r.sev, name: r.name, count: counts.get(r.id) || 0, delta: prevCounts.has(r.id) ? (counts.get(r.id) || 0) - prevCounts.get(r.id) : null }));
    const sorted = ruleRows.filter((r) => r.count).sort((a, b) => SEVRANK[a.sev] - SEVRANK[b.sev] || b.count - a.count);
    const S1 = [];
    S1.push("sweep " + new Date().toISOString().slice(0, 19) + "Z  " + res.ms + " ms  " + res.files + " files " + Math.round(res.bytes / 1048576) + " MB  " + findings.length + " findings (" + delta.length + " new)" + (full ? "" : "  PARTIAL (--only/--files)"));
    S1.push("phases: " + phases.map(([n, t]) => n + " " + t).join(", ") + "  slowest: " + (res.timing || []).slice(0, 4).map(([k, v]) => k + " " + v).join(" ") + "  baseline: " + blNote);
    S1.push("RULES (sev id name count delta), worst first:");
    const zero = ruleRows.filter((r) => !r.count).map((r) => r.id);
    for (const r of sorted.slice(0, 32)) S1.push("  " + r.sev.padEnd(4) + " " + r.id.padEnd(4) + " " + r.name.padEnd(26) + String(r.count).padStart(6) + (r.delta === null ? "" : "  " + (r.delta >= 0 ? "+" : "") + r.delta));
    if (sorted.length > 32) S1.push("  ... " + (sorted.length - 32) + " more rules in sweep.json");
    S1.push("  zero: " + (zero.join(" ") || "-"));
    const m = res.metrics;
    S1.push("metrics: --std headers " + m.stdHeaders + ", std/os importers " + Object.entries(m.stdImports).map(([k, v]) => k + " " + v).join(" ") + ", dyna:file " + (m.file ? m.file.sync + " sync/" + m.file.async + " async" : "?") + ", any+object " + Object.values(m.anyObject).reduce((a, x) => a + x.any + x.object, 0));
    S1.push("STATUS (status.txt):");
    const stPick = st.filter((l) => /^(?:ISSUES appended|ledger|patches|git status|brew|stray|defects baseline|bugs\.tsv|conflict markers)/.test(l) || /^  (?:defects-scan|static-checks|codegraph-report|dod_scan)/.test(l));
    for (const l of stPick.slice(0, 11)) S1.push("  " + l.trim().slice(0, 150));
    S1.push("NEWEST (delta first, then most recently modified files):");
    const newest = delta.length ? delta.slice() : [];
    if (newest.length < 10) {
        const files = [...new Set(findings.map((f) => f.file))].map((p) => [p, mtime(res.root + "/" + p)]).sort((a, b) => b[1] - a[1]).slice(0, 10).map((x) => x[0]);
        for (const p of files) for (const f of findings) if (f.file === p && newest.length < 10 && !newest.includes(f)) newest.push(f);
    }
    for (const f of newest.slice(0, 10)) S1.push("  " + f.rule + " " + f.file + ":" + f.line + "  " + f.snippet.slice(0, 90));
    file.writeFile(out + "/SUMMARY.txt", S1.slice(0, 60).join("\n") + "\n");
    file.writeFile(out + "/sweep.json", JSON.stringify({ version: 1, at: new Date().toISOString(), full, ms: res.ms, phases, files: res.files, bytes: res.bytes, skippedBig: res.skippedBig,
        timing: res.timing, findings: findings.length, new: delta.length, baseline: blNote, rules: ruleRows, metrics: m, rule18: S.r18 || null, status: st }, null, 1) + "\n");
    if (full) {
        file.writeFile(out + "/last-keys.tsv", keys.join("\n") + "\n");
        file.writeFile(out + "/last-counts.tsv", ruleRows.map((r) => r.id + "\t" + r.count).join("\n") + "\n");
    }
    if (o.samples) {
        const SM = [];
        for (const r of all) {
            const hits = findings.filter((f) => f.rule === r.id);
            if (!hits.length) continue;
            const byFile = new Map();
            for (const h of hits) { if (!byFile.has(h.file)) byFile.set(h.file, []); byFile.get(h.file).push(h); }
            const pick = [];
            const lists = [...byFile.values()];
            for (let i = 0; pick.length < o.samples && lists.some((l) => l.length > i); i++) for (const l of lists) if (l[i] && pick.length < o.samples) pick.push(l[i]);
            SM.push("==== " + r.id + " " + r.name + " (" + hits.length + " hits, " + byFile.size + " files)");
            for (const h of pick) {
                SM.push("--- " + h.file + ":" + h.line + "  " + h.snippet.slice(0, 160));
                const f = S.get(h.file);
                if (f && f.ls) for (let k = Math.max(0, h.line - 2); k < Math.min(f.ls.length, h.line + 1); k++) SM.push((k === h.line - 1 ? ">" : " ") + String(k + 1).padStart(5) + " " + f.text(k).slice(0, 150));
            }
        }
        file.writeFile(out + "/samples.txt", SM.join("\n") + "\n");
    }
    return { counts: ruleRows, delta: delta.length };
}

function main() {
    const o = parseArgs(scriptArgs(sys.args()));
    if (o.help) return 0;
    if (o.selftest) return selftest() ? 1 : 0;
    const root = (o.root || sys.cwd()).replace(/\/$/, "");
    const env = (k) => { try { return sys.getEnv(k); } catch { return undefined; } };
    const aw = env("SWEEP_AW") || (file.exists(root + "/.agent-work/ISSUES.md") ? root + "/.agent-work" : file.exists(root + "/../ISSUES.md") ? root.replace(/\/[^/]+$/, "") : root + "/.agent-work");
    if (o.patch) return patchGuard(o, root, aw);
    const out = o.out || env("SWEEP_OUT") || aw + "/sweep";
    const res0 = {};
    const w = walk(root);
    phase("read");
    let fileSet = null;
    if (o.files) {
        const list = o.files.startsWith("@") ? (readText(o.files.slice(1)) || "").split(/\s+/) : o.files.split(",");
        fileSet = new Set(list.map((s) => s.trim().replace(/^\.\//, "")).filter(Boolean));
    }
    const timing = {};
    const { findings: raw, S } = analyze(w.map, w.paths, { only: o.only, timing, aux: walkAux(root, aw), covmap: { map: readHead(root + "/.obj/cov/map.tsv"), merges: readText(aw + "/merges.tsv") || "" }, exists: (p) => file.exists(root + "/" + p) });
    const mask0 = nowMs();
    res0.timing = Object.entries(timing).sort((a, b) => b[1] - a[1]).map(([k, v]) => [k, Math.round(v)]);
    phase("rules+joins");
    let findings = raw;
    if (fileSet) {
        const scopes = new Set();
        for (const p of fileSet) { const m = /^(?:src\/modules|tests)\/([^/]+)\//.exec(p); if (m) scopes.add(m[1]); }
        findings = raw.filter((f) => fileSet.has(f.file) || [...scopes].some((m) => f.file.startsWith("src/modules/" + m + "/") || f.file.startsWith("tests/" + m)));
    }
    findings.sort((a, b) => SEVRANK[a.sev] - SEVRANK[b.sev] || (a.rule < b.rule ? -1 : a.rule > b.rule ? 1 : 0) || (a.file < b.file ? -1 : a.file > b.file ? 1 : a.line - b.line));
    const metricsRes = metrics(S);
    const st = status(root, aw);
    phase("status");
    const res = { timing: res0.timing, ms: 0, files: w.map.size, bytes: w.bytes, skippedBig: w.skippedBig, metrics: metricsRes, status: st, root };
    res.ms = Math.round(nowMs());
    const wr = writeOutputs(o, out, findings, S, res);
    phase("write");
    console.log("sweep: " + findings.length + " findings (" + wr.delta + " new) in " + Math.round(nowMs()) + " ms over " + w.map.size + " files; " + out + "/SUMMARY.txt");
    return 0;
}

let rc = 0;
try { rc = main(); } catch (e) { console.log("sweep: " + (e && e.stack || e)); rc = 2; }
if (rc) { if (typeof sys.exit === "function") sys.exit(rc); else throw new Error("sweep: exit " + rc); }
