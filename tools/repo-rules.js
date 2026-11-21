// flags: --std
// Repository rules that used to live as prose in a local notes file, now executable.
// Each rule names the quiet failure it prevents; `selftest` plants one violation per rule
// (and one near miss that must NOT fire) before any scan is believed.
//
//   ./dynajs --std tools/repo-rules.js selftest   # every rule fires on its plant
//   ./dynajs --std tools/repo-rules.js            # scan tracked + untracked-unignored files
import * as std from "std";
import * as os from "os";

const SHELL = (p) => /(^|\/)[^/]+\.sh$/.test(p) || p === "build.sh";
const CSRC = (p) => /^src\/.*\.(c|h)$/.test(p);

function kitIn(list, f) {
    let hit = false;
    for (const e of list) {
        const neg = e.startsWith("!"), p = neg ? e.slice(1) : e;
        const m = p.endsWith("/") ? f.startsWith(p) : /[*{]/.test(p) ? new RegExp("^" + p.replace(/[.+?^$()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*").replace(/\{([^}]*)\}/g, (_, a) => "(?:" + a.split(",").join("|") + ")") + "$").test(f) : f === p;
        if (m) { if (neg) return false; hit = true; }
    }
    return hit;
}

function lineOf(text, index) {
    let n = 1;
    for (let i = 0; i < index; i++) if (text.charCodeAt(i) === 10) n++;
    return n;
}

function regexRule(re) {
    return (text) => {
        const hits = [];
        const g = new RegExp(re.source, re.flags.includes("g") ? re.flags : re.flags + "g");
        for (let m; (m = g.exec(text)); ) {
            hits.push(lineOf(text, m.index));
            if (m[0].length === 0) g.lastIndex++;
        }
        return hits;
    };
}

const TMP = "/t" + "mp/";
const ARM_WORDS = new Set(["arm", "register", "reserve", "watch", "subscribe"]);

const FILE_RULES = [
    {
        id: "ere-word-boundary",
        why: "\\b is not a word boundary in POSIX ERE: `grep -E '\\bX\\b'` matches nothing and reads as a clean sweep; use grep -w",
        scope: SHELL,
        check: regexRule(/\bgrep\s+-[A-Za-z]*E[A-Za-z]*\s[^|\n]*\\b/),
        fire: "n=$(grep -E \"\\bFoo\\b\" src/x.c | wc -l)\n",
        miss: "n=$(grep -wE \"Foo\" src/x.c | wc -l)\n",
    },
    {
        id: "pipe-masks-status",
        why: "`prog | head; echo $?` reports head's status, so a crash scores as a pass",
        scope: SHELL,
        check: regexRule(/\|\s*(head|tail|tee|grep|sort|wc)\b[^;\n]*;\s*echo\b[^\n]*\$\?/),
        fire: "./dynajs t.js | head -5; echo rc=$?\n",
        miss: "./dynajs t.js > out 2> err; echo rc=$?\n",
    },
    {
        id: "pipefail-grep-q",
        why: "under pipefail `prod | grep -q X` is false when grep -q exits at the first match and prod dies of SIGPIPE (the tailcall stage skipped every binary check this way); use `grep X >/dev/null`",
        scope: SHELL,
        check: (text) => (/pipefail/.test(text) ? regexRule(/(^|[^|])\|\s*grep\s+(-[A-Za-z]+\s+)*-[A-Za-z]*q/)(text) : []),
        fire: "set -o pipefail\nif nm \"$b\" | grep -q js_tc_table; then echo tc; fi\n",
        miss: "set -o pipefail\nif nm \"$b\" | grep js_tc_table >/dev/null || grep -q x f; then echo tc; fi\n",
    },
    {
        id: "subst-before-status",
        why: "a $(...) earlier in the same string resets $?, so `echo \"$(basename $f) rc=$?\"` prints basename's status",
        scope: SHELL,
        check: regexRule(/"[^"\n]*\$\([^)\n]*\)[^"\n]*\$\?/),
        fire: "echo \"$(basename $f) rc=$?\"\n",
        miss: "rc=$?\necho \"$(basename $f) rc=$rc\"\n",
    },
    {
        id: "exclude-list",
        why: "copies are stated as include lists: an exclude list silently takes every new directory nobody told it about",
        scope: SHELL,
        check: regexRule(/\b(rsync|tar)\b[^\n]*--exclude/),
        fire: "rsync -a --exclude=.git /host/ /build/\n",
        miss: "rsync -a --include-from=list.txt /host/ /build/\n",
    },
    {
        id: "if-const",
        why: "no #if 0 / #if 1 blocks: dead code is deleted, live code is unfenced",
        scope: CSRC,
        check: regexRule(/^[ \t]*#[ \t]*if[ \t]+[01][ \t]*$/m),
        fire: "int a;\n#if 0\nint b;\n#endif\n",
        miss: "#if CONFIG_X\nint b;\n#endif\n",
    },
    {
        id: "unsafe-libc",
        why: "P15 secure-c-libs: no unbounded or unchecked libc string calls in src (strcpy, strcat, sprintf, vsprintf, gets, stpcpy, strncpy, strncat, strtok, atoi, atol, atoll, tmpnam, mktemp): use snprintf, memcpy with a checked length, strtol/strtoll with an end pointer, mkstemp/mkdtemp",
        scope: CSRC,
        check: regexRule(/(^|[^A-Za-z0-9_.>])(strcpy|strcat|v?sprintf|gets|stpcpy|strncpy|strncat|strtok|atoi|atoll?|tmpnam|mktemp)\s*\(/m),
        fire: "    strncpy(dst, src, n);\n    n = atoi(e);\n",
        fireCount: 2,
        miss: "    snprintf(dst, n, \"%s\", src);\n    n = strtol(e, &end, 10);\n    p->strcpy(x);\n    my_atoi(e);\n",
    },
    {
        id: "tracked-alloc-member",
        why: "dyna-track.h is force-included into dyn-*.c and #defines free/malloc/...: a member of that name is silently renamed",
        scope: (p) => /^src\/core\/dyn-[^/]+\.(c|h)$/.test(p),
        check: regexRule(/\(\s*\*\s*(free|dup|malloc|realloc|calloc)\s*\)\s*\(/),
        fire: "typedef struct { void (*free)(void*); } ops_t;\n",
        miss: "typedef struct { void (*release)(void*); } ops_t;\n",
    },
    {
        id: "tracked-ab-take",
        why: "a tracked module buffer handed to JS_NewArrayBufferTake is freed by the engine with the plain allocator, so it never leaves the native-memory ledger and later calls hit a false out-of-memory; use dyn_nat_ab_take",
        scope: (p) => /^src\/(modules|core)\/.+\.(c|h)$/.test(p) || (/^src\/dyna?-[^/]+\.c$/.test(p) && p !== "src/dyna-nat.c"),
        check: regexRule(/\bJS_NewArrayBufferTake\s*\(/),
        fire: "    ab = JS_NewArrayBufferTake(ctx, p, n * sizeof(double));\n",
        miss: "    ab = dyn_nat_ab_take(ctx, p, n * sizeof(double));\n",
    },
    {
        id: "time-only-tmp-name",
        why: "a temp path named only by the clock collides when the same suite runs in two gate legs at once (normal, leak and san legs share TMPDIR; four split dyna:file suites shared dj_fbytes.<ms>): the leak leg failed test_file_file_reader on a clobbered file; add Math.random()/getpid() to the name or use makeTempDir()",
        scope: (p) => /^tests\/.+\.js$/.test(p) && !p.startsWith("tests/test262"),
        check: (text) => {
            const hits = [];
            text.split("\n").forEach((l, i) => {
                if (/(TMPDIR|\/tmp\b|tempDir\(\)|Path\.temp\(\))/.test(l) && /\b(Date|performance)\.now\(\)/.test(l)
                    && !/Math\.random|getpid\(\)|\bpid\(\)|randomUUID|\$\$/.test(l)) hits.push(i + 1);
            });
            return hits;
        },
        fire: "const path = new Path(`${std.getenv(\"TMPDIR\") || \"/tmp\"}/dj_file.${Date.now() % 10000000}.txt`);\nconst dir = new Path(Path.temp(), \"dj_fbytes.\" + (Date.now() % 10000000));\n",
        fireCount: 2,
        miss: "const path = new Path(`${std.getenv(\"TMPDIR\") || \"/tmp\"}/dj_file.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.txt`);\nconst T = `${std.getenv(\"TMPDIR\") || \"/tmp\"}/dj_poolcap.${os.getpid()}.${Date.now()}`;\nconst t0 = Date.now();\n",
    },
    {
        id: "std-import-needs-flag",
        why: "a suite, bench or probe that statically imports \"std\" or \"os\" dies at load with \"could not load module filename 'std'\" unless the runner passes --std, and the runners (tools/run-tests-parallel.sh, run2.sh) take it only from a `// flags: --std` line in the first 3 lines (tests/probes/probe_modules.js had only a `Run: ./dynajs --std` comment); put `// flags: --std` on line 1",
        scope: (p) => /^tests\/(?!lib\/|blackbox\/|test262)(?:.+\/)?(test|bench|probe)_[^/]*\.js$/.test(p),
        check: (text) => {
            if (/^(?:.*\n){0,2}\/\/ *flags:.*--std/.test(text)) return [];
            const code = text.replace(/`[^`]*`/g, (s) => s.replace(/[^\n]/g, " "));
            const m = /^import\s[^\n]*\sfrom\s+["'](std|os)["']/m.exec(code);
            return m ? [lineOf(text, m.index)] : [];
        },
        fire: "// Run: ./dynajs --std tests/probes/probe_x.js\nimport * as std from \"std\";\n\n// flags: --std\n",
        miss: "// Run: ./dynajs tests/net/test_x.js\nconst { gc } = await import(\"std\").catch(() => ({}));\nconst r = sh(\"c.mjs\", `\nimport * as os from \"os\";`);\n",
    },
    {
        id: "cpuclock-result-object",
        why: "bestMs()/cpuMs() from tests/lib/cpuclock.js return { ms, value }, not a number: `bestMs(f) * 8` is NaN and `tb <= 8 * ts` is always false, so a scale check goes red for no reason (or a ratio prints NaN); take `.ms` (`bestMs(f, 5).ms`), destructure (`const { ms } = cpuMs(f)`), or keep the object and read only `.ms`/`.value` from it",
        scope: (p) => /^(tests|bench|tools|examples)\/.+\.(js|mjs)$/.test(p) && p !== "tests/lib/cpuclock.js",
        check: (text) => {
            if (!/from\s+["'][^"']*cpuclock\.js["']/.test(text)) return [];
            const code = stripJsComments(text);
            const hits = [];
            for (const m of code.matchAll(/\b(?:bestMs|cpuMs)\(/g)) {
                if (/function\s+$/.test(code.slice(Math.max(0, m.index - 12), m.index))) continue;
                let i = m.index + m[0].length, depth = 1, q = "";
                for (; i < code.length && depth; i++) {
                    const ch = code[i];
                    if (q) { if (ch === "\\") i++; else if (ch === q) q = ""; }
                    else if (ch === "\"" || ch === "'" || ch === "`") q = ch;
                    else if (ch === "(") depth++;
                    else if (ch === ")") depth--;
                }
                if (/^\s*\.\s*(ms|value)\b/.test(code.slice(i, i + 16))) continue;
                const before = code.slice(Math.max(0, m.index - 200), m.index);
                if (/\}\s*=\s*$/.test(before) || /(\breturn|=>)\s*$/.test(before)) continue;
                const asg = before.match(/\b(?:const|let|var)?\s*([A-Za-z_$][\w$]*)\s*=\s*$/);
                if (asg) {
                    const name = asg[1].replace(/\$/g, "\\$");
                    const rest = code.slice(i);
                    const uses = [...rest.matchAll(new RegExp("(?<![\\w$.])" + name + "(?![\\w$])(\\s*\\.\\s*\\w+)?", "g"))];
                    if (uses.every((u) => /^\s*\.\s*(ms|value)$/.test(u[1] || ""))) continue;
                }
                hits.push(lineOf(code, m.index));
            }
            return hits;
        },
        fire: "import { bestMs, cpuMs } from \"../lib/cpuclock.js\";\nconst ts = bestMs(() => small.serialize(), 5), tb = bestMs(() => big.serialize(), 5);\nok(tb <= 8 * ts, \"linear\");\n" +
            "if (cpuMs(() => f()) > 50) fail(\"slow\");\nconst r = cpuMs(work);\nprint(r.toFixed(1));\n",
        fireCount: 4,
        miss: "import { bestMs, cpuMs } from \"../lib/cpuclock.js\";\nok(bestMs(() => f(), 3).ms < 5, \"fast\");\nconst ts = bestMs(() => small.serialize(), 5).ms, tb = bestMs(() => big.serialize(), 3).ms;\n" +
            "const { ms: dt, value: m } = bestMs(() => err(f));\nconst { ms } = cpuMs(() => { n = g(\"(\"); });\n" +
            "const r = cpuMs(work);\nprint(r.ms.toFixed(1), r.value);\nconst time = (f) => cpuMs(f);\nfunction best(f) { return bestMs(f, 3); }\nexport function bestMs(fn, n = 3) { return 0; }\n",
    },
    {
        id: "persist-path-type",
        why: "save/load of every dyna:structures class takes a dyna:file Path and throws TypeError on a string, so a d.ts signature typed `path: string` promises a call that always fails",
        scope: (p) => p === "dynajs.d.ts",
        check: regexRule(/^.*\bpath: string\b.*\bPersist(Load|Save)Options\b.*$/m),
        fire: "        static load<T>(this: new () => Deque<T>, path: string, opts?: PersistLoadOptions): Deque<T>;\n",
        miss: "        static load<T>(this: new () => Deque<T>, path: import(\"dyna:file\").Path, opts?: PersistLoadOptions): Deque<T>;\n",
    },
    {
        id: "discarded-arm",
        why: "a (void)-cast on a call that ARMS/REGISTERS/RESERVES/WATCHES deletes the only signal that the mechanism is not running",
        scope: CSRC,
        check: (text) => {
            const hits = [];
            const g = /\(void\)\s*([A-Za-z_]\w*)\s*\(/g;
            for (let m; (m = g.exec(text)); ) {
                if (m[1].split("_").some((w) => ARM_WORDS.has(w))) hits.push(lineOf(text, m.index));
            }
            return hits;
        },
        fire: "static void f(a_t* a)\n{\n    (void)uaio_arm_timer(a);\n}\n",
        miss: "static void f(a_t* a)\n{\n    (void)dyn_aio_pool_warm(a);\n}\n",
    },
    {
        id: "struct-hack",
        why: "a trailing `x[1]` member written past its end is UB under -fstrict-flex-arrays=2 (the engine's flags); use a C99 flexible member",
        scope: CSRC,
        check: regexRule(/\[1\];[ \t]*\n[ \t]*\}[ \t]*[A-Za-z_]*[ \t]*;/),
        fire: "typedef struct {\n    int n;\n    char name[1];\n} rec_t;\n",
        miss: "typedef struct {\n    int n;\n    char name[];\n} rec_t;\n",
    },
    {
        id: "strict-c17-consts",
        why: "dyn-ds is compiled alone at -std=c17 on glibc, where M_PI and friends are hidden (an XSI extension)",
        scope: (p) => /^src\/core\/dyn-ds\.(c|h)$/.test(p),
        check: regexRule(/\bM_(PI|PI_2|PI_4|E|SQRT2|SQRT1_2|LN2|LN10|LOG2E|LOG10E)\b/),
        fire: "double r = 2 * M_PI;\n",
        miss: "double r = 2 * DS_PI;\n",
    },
    {
        id: "doc-tense",
        why: "the reference states what the API is, in the present tense: no TODO, no 'not yet implemented', no 'for now'",
        scope: (p) => p === "dynajs.d.ts" || p === "README.md" || p === "API.md",
        check: regexRule(/\b(TODO|FIXME|XXX|not yet (implemented|supported)|for now|deferred)\b/i),
        fire: "/** Streams the body. Compression is not yet supported. */\n",
        miss: "/** Commands issued but not yet answered. */\n",
    },
    {
        id: "async-twin",
        why: "no ...Async twins of blocking natives (owner, 2026-10-08): run blocking work on an os.Worker",
        scope: (p) => p === "dynajs.d.ts",
        check: (text) => {
            const hits = [];
            const g = /\b([a-z][A-Za-z0-9]*)Async\s*[<(]/g;
            const standard = new Set(["dispose", "from"]);
            for (let m; (m = g.exec(text)); ) if (!standard.has(m[1])) hits.push(lineOf(text, m.index));
            return hits;
        },
        fire: "    function readFileAsync(path: string): Promise<string>;\n",
        miss: "    disposeAsync(): Promise<void>;\n    fromAsync<T>(it: AsyncIterable<T>): Promise<T[]>;\n",
    },
    {
        id: "test-fixed-port",
        why: "suites run side by side (san || leaks): a server in a test binds port 0 and reads back the real port",
        scope: (p) => /^tests\/.*\.js$/.test(p),
        check: regexRule(/(Server(Async)?|\blisten)\s*\(\s*\{[^}]*\bport\s*:\s*[1-9]\d{2,4}\b/),
        fire: "const srv = new HTTPServer({ port: 8080, routes });\n",
        miss: "const srv = new HTTPServer({ port: 0, routes });\nconst c = TCPServer.connect({ host: \"10.255.255.1\", port: 5432 });\ns.listen(128)\n",
    },
    {
        id: "test-wall-clock-budget",
        why: "an assert that a wall-clock duration stays under a fixed number of ms flakes under a parallel or sanitizer run (B19); time CPU work with cpuNow()/bestMs() from tests/lib/cpuclock.js or scale the bound with hostScale(), and give a check that really measures wall time (a deadline against a hang, timer accuracy) a `wall-clock:` comment with the reason on or up to 3 lines above",
        scope: (p) => /^tests\/.*\.js$/.test(p) && !/^tests\/(lib|agent)\//.test(p),
        check: (text) => {
            const code = stripJsComments(text);
            const raw = text.split("\n"), lines = code.split("\n");
            const assigns = [];
            lines.forEach((l, i) => {
                for (const m of l.matchAll(/([A-Za-z_$][\w$]*)\s*=\s*\(?\s*(?:performance\.now|Date\.now)\(\)\s*-\s*[A-Za-z_$]/g)) assigns.push({ v: m[1], i });
            });
            const hits = [];
            lines.forEach((l, i) => {
                if (!/\b(ok|check|assert\w*|expect|eq|must|t\.\w+|a\.ok)\s*\(/.test(l)) return;
                if (/hostScale|cpuMs|bestMs|spent\(|cpuNow|wallWithin/.test(l)) return;
                let hit = /(?:performance\.now|Date\.now)\(\)\s*-\s*[\w$.]+\s*<=?\s*[\d_]/.test(l);
                for (const a of assigns) if (a.i <= i && i - a.i <= 12 && new RegExp("(^|[^\\w$.])" + a.v.replace(/\$/g, "\\$") + "\\s*<=?\\s*[\\d_]").test(l)) hit = true;
                if (!hit) return;
                if (raw.slice(Math.max(0, i - 3), i + 1).some((r) => /\/\/\s*wall-clock:/.test(r))) return;
                hits.push(i + 1);
            });
            return hits;
        },
        fire: "const t0 = Date.now();\nconst r = R(x).all();\nok(Date.now() - t0 < 2000, \"quick\");\nconst t1 = performance.now();\nrun();\nconst dt = performance.now() - t1;\nassert(dt < 500, \"fast\");\n",
        fireCount: 2,
        miss: "const t0 = cpuNow();\nrun();\nok(cpuNow() - t0 < 2000, \"quick\");\nconst t1 = Date.now();\nawait sleep(5);\nconst dt = Date.now() - t1;\n// wall-clock: the deadline against a hang\nok(dt < 5000, \"prompt\");\nok(dt2 < 9 * hostScale(), \"scaled\");\nwhile (Date.now() - t1 < 3000) await sleep(5);\n",
    },
    {
        id: "test-wall-clock-ratio",
        why: "a complexity or cost-ratio check on CPU work timed with performance.now()/Date.now() measures host load, not the code (a san leg at load 60 read wide indexOf 24 ms at 40k vs 273 ms at 160k and failed 'scales linearly'); time the work with cpuNow()/bestMs() from tests/lib/cpuclock.js, or put a `wall-clock:` comment (with the reason) on or up to 3 lines above a check that really measures wall time",
        scope: (p) => /^tests\/.*\.js$/.test(p) && !p.startsWith("tests/lib/"),
        check: (text) => {
            const code = stripJsComments(text);
            const durs = [];
            for (const m of code.matchAll(/(?:performance\.now|Date\.now)\(\)\s*-\s*[A-Za-z_$]/g)) durs.push(lineOf(code, m.index) - 1);
            if (!durs.length) return [];
            const raw = text.split("\n");
            const hits = [];
            code.split("\n").forEach((l, i) => {
                if (!/\b(ok|check|assert\w*|expect|eq|t\.\w+)\s*\(/.test(l)) return;
                const scale = /\b(scales?|scaling|linear(ly)?|quadratic|sub-?quadratic|superlinear|O\(n[^)]*\))/i.test(l);
                const ratio = /[\w\])]\s*<\s*(?:Math\.max\(\s*)?[\w.]+(?:\s*,\s*[\d.]+\s*\))?\s*\*\s*\d|\b\w+\s*\/\s*\w+\s*<\s*\d/.test(l);
                if (!scale && !ratio) return;
                if (!durs.some((d) => d <= i && i - d <= 12)) return;
                if (raw.slice(Math.max(0, i - 3), i + 1).some((r) => /\/\/\s*wall-clock:/.test(r))) return;
                hits.push(i + 1);
            });
            return hits;
        },
        fire: "const t0 = Date.now();\nconst j = left.JOIN(right, \"k\", \"k\", \"inner\");\nconst dt = Date.now() - t0;\nok(dt < 500, `join index build is O(n): ${dt} ms`);\n" +
            "const time = (src) => { const t0 = performance.now(); const o = JSON.parse(src); return [performance.now() - t0, o]; };\n" +
            "const [tc] = time(ctl), [tx] = time(col);\nok(tx < Math.max(tc, 1) * 10, \"crafted keys do not flood the atom table\");\n",
        fireCount: 2,
        miss: "const cost = (n) => {\n    const t0 = cpuNow();\n    for (let r = 0; r < 3; r++) h.indexOf(nd);\n    return cpuNow() - t0;\n};\n" +
            "const c1 = cost(4e4), c4 = cost(16e4);\nok(c4 < Math.max(c1, 0.05) * 10, \"wide indexOf scales linearly\");\n" +
            "const t = Date.now();\nawait sleep(50);\nconst dt = Date.now() - t;\nok(dt >= 45, \"sleep waited\");\n" +
            "const w0 = performance.now();\nconst r = bench(f);\nconst w = performance.now() - w0;\n// wall-clock: bench() reports a wall-clock rate\nok(r.ms < w * 4, \"bench agrees with the wall clock\");\n",
    },
    {
        id: "test-background-helper",
        why: "a helper backgrounded with sh(`cmd &`) is reparented to pid 1 the moment its shell returns, so a killed or failed suite leaves it running for days (~290 orphaned mock servers once); start it with spawnHelper() from tests/lib/helper.js",
        scope: (p) => /^tests\/.*\.js$/.test(p) && p !== "tests/lib/helper.js",
        check: (text) => {
            const hits = [];
            for (const m of text.matchAll(/(?<=\s)&(?!&)\s*(?:["'`]\s*[,)\]+;]|echo\s+\$!)/g)) {
                let from = m.index;
                for (let k = 0; k < 3 && from > 0; k++) from = text.lastIndexOf("\n", from - 1);
                if (/\b(sh|Exec|exec|execSync|system|Spawn)\s*\(|"-c"/.test(text.slice(Math.max(from, 0), m.index)))
                    hits.push(lineOf(text, m.index));
            }
            return hits;
        },
        fire: "sh(`python3 ${P(\"mock.py\")} >${P(\"mock.log\")} 2>&1 &`);\n" +
            "Exec(\"/bin/sh\", [\"-c\", \"python3 \" + T + \"/srv.py > \" + T + \"/log 2>&1 &\"]);\n" +
            "sh(`rm -f ${pf}; ${cmd} >${log} 2>&1 & echo $! > ${pf}`);\n",
        miss: "spawnHelper(`python3 ${P(\"mock.py\")} >${P(\"mock.log\")} 2>&1`);\n" +
            "eq(HTMLParse(\"<p>a &amp b</p>\")[0], \"a &amp b\", \"and so does a bare &\");\n" +
            "sh(\"make a && make b\");\nnew Spawn(\"/bin/sh\", [\"-c\", \"(trap '' TERM; exec sleep 8) & wait\"]);\n",
    },
    {
        id: "fixed-tmp-path",
        why: "a literal /tmp/<name> is shared by every tree and user on the host: two runs at once clobber or unlink each other's file (check-error-identifiers did); use mktemp, $$, or tempfile",
        scope: (p) => p === "build.sh" || /^tools\/.*\.(sh|py|js|mjs)$/.test(p),
        check: (text) => {
            const hits = [];
            const lines = text.split("\n");
            const g = new RegExp("(^|[^\\w}.])" + TMP + "[A-Za-z0-9_.][^\\s\"'`;)|>]*", "g");
            for (let i = 0; i < lines.length; i++) {
                if (/^\s*(#|\/\/)/.test(lines[i])) continue;
                g.lastIndex = 0;
                for (let m; (m = g.exec(lines[i])); ) if (!/\$\$|XXX|RANDOM|BASHPID|pid/i.test(m[0])) hits.push(i + 1);
            }
            return hits;
        },
        fire: "    path = \"" + TMP + "_check_error_ids.mjs\"\n",
        miss: "log=\"" + TMP + "x-$$.log\"\nd=$(mktemp -d \"" + TMP + "dyna_XXXXXX\")\nf=\"${TMPDIR:-/tmp}/a.js\"\nmkdir -p \"$LOG/tmp/linux\" \"${OUT}/tmp/a\" ./tmp/b\n# e.g. --out " + TMP + "ws/out\n",
    },
];

const SUITE = (p) =>
    /^tests\/(?!lib\/|corpus\/|probes\/|review_term\/)(.+\/)?(test|bb|oracle|probe|review|sec)_[^/]*\.js$/.test(p) &&
    !/_(child|worker|module)\.js$/.test(p);
const COVERS_LEGACY = "tools/covers-legacy.txt";
const COVERS_UNCOVERED_CAP = 0;
const COVERS_CATEGORIES = new Set(["integration", "blackbox", "example", "contract"]);

function stripJsComments(text) {
    return text.replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, " ")).replace(/\/\/[^\n]*/g, "");
}

function dtsModel(text) {
    const src = stripJsComments(text);
    const model = new Map();
    const head = /\ndeclare module "([^"]+)"\s*\{/g;
    for (let m; (m = head.exec(src)); ) {
        let depth = 1;
        let i = head.lastIndex;
        while (depth && i < src.length) {
            const c = src[i++];
            if (c === "{") depth++;
            else if (c === "}") depth--;
        }
        const body = src.slice(head.lastIndex, i - 1);
        const mod = model.get(m[1]) || new Map();
        model.set(m[1], mod);
        const top = /^ {4}(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(class|function|const|let|var|enum|namespace)\s+([A-Za-z_$][\w$]*)/gm;
        for (let d; (d = top.exec(body)); ) {
            const ent = mod.get(d[2]) || new Set();
            mod.set(d[2], ent);
            const obj = d[1] === "const" && /^\s*:\s*\{/.test(body.slice(top.lastIndex));
            if (d[1] !== "class" && d[1] !== "namespace" && !obj) continue;
            const open = body.indexOf("{", top.lastIndex);
            if (open < 0) continue;
            let dep = 1;
            let k = open + 1;
            while (dep && k < body.length) {
                const c = body[k++];
                if (c === "{") dep++;
                else if (c === "}") dep--;
            }
            const inner = body.slice(open + 1, k - 1);
            const mem = /^ {8}(?:export\s+)?(?:static\s+)?(?:readonly\s+)?(?:get\s+|set\s+)?(?:async\s+)?(?:function\s+|const\s+|class\s+)?\*?([A-Za-z_$][\w$]*)\s*[(<?:=]/gm;
            for (let x; (x = mem.exec(inner)); ) ent.add(x[1]);
        }
    }
    return model;
}

function coversHeader(text) {
    const lines = text.split("\n", 3);
    for (const l of lines) {
        const m = /^\/\/\s*covers:\s*(.*)$/.exec(l.trim());
        if (m) return m[1];
    }
    return null;
}

function checkCovers(path, text, model, exists) {
    const body = coversHeader(text);
    if (body === null) return [`${path}: no "// covers:" header in the first 3 lines`];
    const [targetsPart, reason] = body.split(/\s+--\s+/, 2);
    const toks = targetsPart.trim().split(/[\s,]+/).filter((t) => t.length);
    const out = [];
    let category = null;
    if (toks.length && /^[a-z]+:$/.test(toks[0]) === false && COVERS_CATEGORIES.has(toks[0])) category = toks.shift();
    if (!toks.length) return [`${path}: "// covers:" names nothing`];
    if (toks.length > 1) {
        if (!category) out.push(`${path}: covers ${toks.length} symbols; one per file, or the escape "// covers: integration|blackbox|example|contract <symbols> -- <reason>"`);
        else if (!reason || reason.trim().length < 12) out.push(`${path}: a multi-symbol "${category}" covers header needs " -- <reason>" (12+ characters)`);
    }
    const code = stripJsComments(text.split("\n").filter((l) => !/^\/\/\s*covers:/.test(l.trim())).join("\n"));
    if (toks.length === 1 && !category) {
        const m1 = /^(?:dyna:)?([^.\s:]+)\.([A-Za-z_$][\w$]*)(?:\.([A-Za-z_$][\w$]*))?$/.exec(toks[0]);
        const base = path.split("/").pop();
        if (m1 && /^test_/.test(base)) {
            const snake = (x) => x.replace(/([a-z0-9])([A-Z])/g, "$1_$2").replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2").toLowerCase();
            const want = `test_${m1[1]}_${snake(m1[2])}${m1[3] ? "_" + snake(m1[3]) : ""}`;
            if (base !== want + ".js" && !base.startsWith(want + "_")) out.push(`${path}: covers ${toks[0]}, so its name is ${want}[_<aspect>].js`);
        }
    }
    for (const t of toks) {
        const wide = /^([^.\s]+)\.\*$/.exec(t);
        if (wide) {
            if (!category) out.push(`${path}: covers ${t} (the whole module) only under a category: "// covers: contract ${t} -- <reason>"`);
            else if (!model.has(wide[1])) out.push(`${path}: covers ${t}: dynajs.d.ts declares no module "${wide[1]}"`);
            else if (!code.includes(wide[1]) && !code.includes(`"${wide[1].replace(/^dyna:/, "")}"`)) out.push(`${path}: covers ${t} but never names the module`);
            continue;
        }
        if (t.startsWith("c:")) {
            if (!exists(t.slice(2))) out.push(`${path}: covers ${t}: no such source file`);
            continue;
        }
        const m = /^([^.\s]+(?::[^.\s]+)?)\.([A-Za-z_$][\w$]*)(?:\.([A-Za-z_$][\w$]*))?$/.exec(t);
        if (!m) {
            out.push(`${path}: covers ${t}: not <module>.<symbol>[.<member>] or c:<src path>`);
            continue;
        }
        const [, mod, sym, member] = m;
        const decl = model.get(mod);
        if (!decl) {
            out.push(`${path}: covers ${t}: dynajs.d.ts declares no module "${mod}"`);
            continue;
        }
        if (!decl.has(sym)) {
            out.push(`${path}: covers ${t}: "${mod}" declares no ${sym} in dynajs.d.ts`);
            continue;
        }
        if (member && !decl.get(sym).has(member)) {
            out.push(`${path}: covers ${t}: ${mod} ${sym} declares no member ${member} in dynajs.d.ts`);
            continue;
        }
        const q = mod.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
        if (!new RegExp(`(from\\s*|import\\(\\s*)["']${q}["']`).test(code)) {
            out.push(`${path}: covers ${t} but never imports "${mod}"`);
            continue;
        }
        const names = [sym];
        for (const im of code.matchAll(new RegExp(`import\\s*\\{([^}]*)\\}\\s*from\\s*["']${q}["']`, "g")))
            for (const a of im[1].matchAll(new RegExp(`(?<![\\w$])${sym}\\s+as\\s+([\\w$]+)`, "g"))) names.push(a[1]);
        const rest = code.replace(/import\s[\s\S]*?from\s*["'][^"']+["']/g, "");
        if (!names.some((nm) => new RegExp(`(?<![\\w$])${nm.replace(/\$/g, "\\$")}(?![\\w$])`).test(rest))) {
            out.push(`${path}: covers ${t} but never uses ${sym}`);
            continue;
        }
        if (member && !new RegExp(`\\.\\s*${member}(?![\\w$])`).test(code)) out.push(`${path}: covers ${t} but never uses .${member}`);
    }
    return out;
}

function coversRule(repo) {
    const dts = repo.read("dynajs.d.ts");
    if (dts === null) return ["dynajs.d.ts is missing: the covers headers have nothing to resolve against"];
    const model = dtsModel(dts);
    const fileset = new Set(repo.files);
    const legacyText = repo.read(COVERS_LEGACY) || "";
    const legacy = new Set(legacyText.split("\n").map((l) => l.trim()).filter((l) => l && !l.startsWith("#")));
    const found = [];
    const cap = repo.coversCap ?? COVERS_UNCOVERED_CAP;
    let uncovered = 0;
    for (const p of legacy) if (!fileset.has(p)) found.push(`${COVERS_LEGACY}: ${p} no longer exists -- drop the line`);
    for (const p of repo.files) {
        if (!SUITE(p)) continue;
        const text = repo.read(p);
        if (text === null) continue;
        const hasHeader = coversHeader(text) !== null;
        if (legacy.has(p)) {
            if (hasHeader) found.push(`${COVERS_LEGACY}: ${p} now has a covers header -- drop the line (the list only shrinks)`);
            else uncovered++;
            continue;
        }
        for (const f of checkCovers(p, text, model, (q) => fileset.has(q))) found.push(f);
    }
    if (uncovered > cap) found.push(`${uncovered} suites have no covers header; that count only shrinks (cap ${cap}; 795 on 2026-11-15): split them with tools/split-suite.js, never add to ${COVERS_LEGACY}`);
    return found;
}

function cBlockEnd(text, open) {
    let d = 0;
    for (let i = open; i < text.length; i++) {
        if (text[i] === "{") d++;
        else if (text[i] === "}" && --d === 0) return i;
    }
    return text.length;
}

function selfRefStructs(files, read) {
    const alias = new Map();
    const texts = new Map();
    for (const p of files) {
        const t = read(p);
        if (t === null) continue;
        const text = t.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, (m) => m.replace(/[^\n]/g, " "));
        texts.set(p, text);
        for (const m of text.matchAll(/typedef\s+struct\s+(\w+)\s+(\w+)\s*;/g)) alias.set(m[2], m[1]);
        for (const m of text.matchAll(/typedef\s+struct\s+(\w+)\s*\{/g)) {
            const end = cBlockEnd(text, text.indexOf("{", m.index));
            const t2 = /^\}\s*(\w+)/.exec(text.slice(end));
            if (t2) alias.set(t2[1], m[1]);
        }
    }
    const out = [];
    for (const [p, text] of texts) {
        for (const m of text.matchAll(/struct\s+(\w+)\s*\{/g)) {
            const open = text.indexOf("{", m.index);
            const end = cBlockEnd(text, open);
            const body = text.slice(open + 1, end);
            const names = new Set([m[1]]);
            for (const [a, tag] of alias) if (tag === m[1]) names.add(a);
            const t2 = /^\}\s*(\w+)/.exec(text.slice(end));
            if (t2) names.add(t2[1]);
            const fields = [];
            for (const f of body.matchAll(/(?:^|[;{\n])\s*(?:const\s+)?(?:struct\s+)?(\w+)\s*((?:\*\s*)+\w+\s*(?:\[[^\]]*\])?\s*(?:,\s*\**\s*\w+\s*(?:\[[^\]]*\])?\s*)*);/g))
                if (names.has(f[1])) for (const d of f[2].split(",")) if (d.includes("*")) fields.push(d.replace(/[*\s]/g, ""));
            if (fields.length) out.push({ path: p, line: lineOf(text, m.index), name: m[1], fields });
        }
    }
    return out;
}

function selfRefRule(repo) {
    const CORE = (p) => /^src\/core\/.*\.(c|h)$/.test(p);
    const allowText = repo.read("tools/core-selfref-allow.txt") ?? "";
    const allow = new Map();
    const found = [];
    allowText.split("\n").forEach((l, i) => {
        if (!l.trim() || l.startsWith("#")) return;
        const m = /^(\S+):(\w+)\s+(\S.*)?$/.exec(l);
        if (!m || !m[3]) found.push(`tools/core-selfref-allow.txt:${i + 1}: entry needs "path:Struct reason"`);
        else allow.set(m[1] + ":" + m[2], i + 1);
    });
    const seen = new Set();
    for (const s of selfRefStructs(repo.files.filter(CORE), repo.read)) {
        const key = s.path + ":" + s.name;
        seen.add(key);
        if (!allow.has(key)) found.push(`${s.path}:${s.line}: struct ${s.name} links to itself through ${s.fields.join(", ")}; use u32 indices into an array pool (dyn_parr) or allowlist it with a reason`);
    }
    for (const [key, line] of allow) if (!seen.has(key)) found.push(`tools/core-selfref-allow.txt:${line}: ${key} has no self-referential pointer any more; delete the entry`);
    return found;
}

function danglingTestRefRule(repo) {
    const out = [];
    const have = new Set(repo.files);
    const scripts = repo.files.filter((p) => p === "build.sh" || /^(docker|tools)\/[^/]+\.sh$/.test(p));
    for (const p of scripts) {
        const text = repo.read(p);
        if (text === null) continue;
        const g = /(?<![\w./*-])tests\/[A-Za-z0-9_./-]+\.js\b/g;
        for (let m; (m = g.exec(text)); ) {
            const ref = m[0];
            if (ref === "tests/t.js" || have.has(ref)) continue;
            out.push(`${p}:${lineOf(text, m.index)}: ${ref} does not exist`);
        }
    }
    return out;
}

function moduleOf(p) {
    let m = /^src\/modules\/([^/]+)\//.exec(p);
    if (m) return "src/modules/" + m[1];
    m = /^src\/dyna-([a-z0-9]+)(?:[-.][^/]*)?\.(?:c|h)$/.exec(p);
    if (m && m[1] !== "cli" && m[1] !== "fiber") return "src/dyna-" + m[1];
    return null;
}

function cLiteralBytes(text) {
    let n = 0;
    const g = /"((?:[^"\\\n]|\\.)*)"/g;
    for (let m; (m = g.exec(text)); ) n += m[1].replace(/\\(x[0-9a-fA-F]{1,2}|[0-7]{1,3}|.)/g, "_").length;
    return n;
}

function stripComments(text) {
    return text.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, (s) => s.replace(/[^\n]/g, " "));
}

function evalSites(p, text) {
    const out = [];
    const src = stripComments(text);
    const g = /\bJS_Eval(?:This)?\s*\(\s*\w+\s*,\s*(?:\([^)]*\)\s*)?([^,]+?)\s*,/g;
    for (let m; (m = g.exec(src)); ) {
        const arg = m[1].trim();
        let bytes = 0;
        if (arg.startsWith('"')) bytes = cLiteralBytes(arg);
        else if (/^\w+$/.test(arg)) {
            const def = new RegExp("\\b" + arg + "\\s*(?:\\[\\s*\\w*\\s*\\])?\\s*=\\s*((?:\\s*\"(?:[^\"\\\\\\n]|\\\\.)*\")+)").exec(src);
            const fmt = new RegExp("\\bsnprintf\\s*\\(\\s*" + arg + "\\s*,[^,]+,((?:\\s*\"(?:[^\"\\\\\\n]|\\\\.)*\")+)").exec(src);
            bytes = def ? cLiteralBytes(def[1]) : fmt ? cLiteralBytes(fmt[1]) : 0;
        }
        out.push({ path: p, line: lineOf(text, m.index), arg: /^\w+$/.test(arg) ? arg : "<expr>", bytes });
    }
    return out;
}

function readAllow(repo, file, re, shape) {
    const allow = new Map();
    const found = [];
    (repo.read(file) ?? "").split("\n").forEach((l, i) => {
        if (!l.trim() || l.startsWith("#")) return;
        const m = re.exec(l);
        if (!m) found.push(`${file}:${i + 1}: entry needs "${shape}"`);
        else allow.set(m[1], { line: i + 1, m });
    });
    return { allow, found };
}

function runtimeEvalRule(repo) {
    const FILE = "tools/runtime-eval-allow.txt";
    const { allow, found } = readAllow(repo, FILE, /^(\S+ \S+)\s+\S.*$/, "path firstArg reason");
    const seen = new Set();
    for (const p of repo.files.filter((f) => moduleOf(f) && /\.c$/.test(f))) {
        const text = repo.read(p);
        if (text === null) continue;
        for (const s of evalSites(p, text)) {
            const key = s.path + " " + s.arg;
            seen.add(key);
            if (!allow.has(key)) found.push(`${s.path}:${s.line}: JS_Eval of ${s.arg} at run time; build the behaviour in C (native functions, promise reactions) or allowlist it with a reason`);
        }
    }
    for (const [key, e] of allow) if (!seen.has(key)) found.push(`${FILE}:${e.line}: ${key} no longer evaluates JS; delete the entry`);
    return found;
}

function shippedJsRule(repo) {
    const FILE = "tools/shipped-js-allow.txt";
    const CAP = 5;
    const { allow, found } = readAllow(repo, FILE, /^(\S+)\s+([0-9]+(?:\.[0-9])?)%\s+\S.*$/, "module max% reason");
    const js = new Map(), all = new Map();
    for (const p of repo.files) {
        const mod = moduleOf(p);
        if (!mod || !/\.(c|h)$/.test(p)) continue;
        const text = repo.read(p);
        if (text === null) continue;
        all.set(mod, (all.get(mod) ?? 0) + text.length);
        const inc = /\.js\.inc\.c$/.test(p) ? cLiteralBytes(stripComments(text)) : 0;
        const ev = inc ? 0 : evalSites(p, text).reduce((a, s) => a + s.bytes, 0);
        js.set(mod, (js.get(mod) ?? 0) + inc + ev);
    }
    for (const [mod, total] of all) {
        const pct = Math.round((1000 * (js.get(mod) ?? 0)) / total) / 10;
        const e = allow.get(mod);
        if (e) {
            const max = +e.m[2];
            if (pct > max) found.push(`${mod}: ships ${pct}% JS, above its allowlisted ${max}%`);
            else if (pct <= CAP) found.push(`${FILE}:${e.line}: ${mod} ships ${pct}% JS, within the ${CAP}% cap; delete the entry`);
            else if (max - pct >= 0.5) found.push(`${FILE}:${e.line}: ${mod} ships ${pct}% JS; lower the entry from ${max}% (the list only shrinks)`);
        } else if (pct > CAP) found.push(`${mod}: ships ${pct}% JS source (eval'd at run time), above the ${CAP}% cap; port it to C or allowlist it with a reason`);
    }
    for (const [mod, e] of allow) if (!all.has(mod)) found.push(`${FILE}:${e.line}: ${mod} does not exist; delete the entry`);
    return found;
}

const REPO_RULES = [
    {
        id: "no-runtime-eval",
        why: "a module builds its behaviour in C (native functions, promise reactions, class callbacks), never by evaluating JS source at run time: eval'd JS costs a parse per runtime, hides from the static checks and the d.ts contract, and its property lookups are what the C ports removed; every remaining JS_Eval in src/modules or src/dyna-*.c is listed in tools/runtime-eval-allow.txt as \"path firstArg reason\", a list that only shrinks",
        check: runtimeEvalRule,
        fire: {
            files: ["src/modules/a/a.c", "src/dyna-b.c", "tools/runtime-eval-allow.txt"],
            ignored: () => false,
            read: (p) => ({
                "src/modules/a/a.c": "static const char js[] = \"1+1\";\nJSValue f(JSContext* ctx) {\n    return JS_Eval(ctx, js, 3, \"<a>\", 0);\n}\n",
                "src/dyna-b.c": "    x = JS_Eval(ctx, \"(function () {})\", 15, \"<b>\", 0);\n",
                "tools/runtime-eval-allow.txt": "src/modules/a/gone.c js old bridge\nsrc/dyna-b.c\n",
            })[p] ?? null,
        },
        miss: {
            files: ["src/modules/a/a.c", "src/core/k.c", "src/dyna-cli.c", "src/dyna-c.c", "tools/runtime-eval-allow.txt"],
            ignored: () => false,
            read: (p) => ({
                "src/modules/a/a.c": "static const char js[] = \"1+1\";\n/* was JS_Eval(ctx, other, 1, \"\", 0) */\nJSValue f(JSContext* ctx) {\n    return JS_Eval(ctx, js, 3, \"<a>\", 0);\n}\n",
                "src/core/k.c": "    v = JS_Eval(ctx, buf, n, \"<k>\", 0);\n",
                "src/dyna-cli.c": "    v = JS_Eval(ctx, buf, len, filename, 0);\n",
                "src/dyna-c.c": "    v = JS_EvalFunction(ctx, fn);\n",
                "tools/runtime-eval-allow.txt": "# path firstArg reason\nsrc/modules/a/a.c js the loop stays JS until its port\n",
            })[p] ?? null,
        },
        fires: [/a\.c:3: JS_Eval of js at run time/, /dyna-b\.c:1: JS_Eval of <expr> at run time/, /allow\.txt:1: src\/modules\/a\/gone\.c js no longer evaluates JS/, /allow\.txt:2: entry needs "path firstArg reason"/],
    },
    {
        id: "shipped-js-cap",
        why: "a module is C with at most 5% of its source bytes being JS it evaluates at run time (string literals passed to JS_Eval, *.js.inc.c sources); a module above the cap is listed in tools/shipped-js-allow.txt as \"module max% reason\" and the entry may only go down (a 0.5-point drop below it must be written back)",
        check: shippedJsRule,
        fire: {
            files: ["src/modules/m/m.c", "src/dyna-q.c", "src/dyna-q.js.inc.c", "src/modules/k/k.c", "src/modules/s/s.c", "tools/shipped-js-allow.txt"],
            ignored: () => false,
            read: (p) => ({
                "src/modules/m/m.c": "static const char js[] = \"(function () { return globalThis.x + 1; })\";\nv = JS_Eval(ctx, js, 1, \"<m>\", 0);\n",
                "src/dyna-q.c": "#include \"dyna-q.js.inc.c\"\n",
                "src/dyna-q.js.inc.c": "static const char q_src[] = \"(function () { const a = [1, 2, 3]; return a.map((x) => x * 2); })\";\n",
                "src/modules/k/k.c": "static const char kjs[] = \"(function () { return 1; })\";\nv = JS_Eval(ctx, kjs, 27, \"<k>\", 0);\n" + "int pad;\n".repeat(4),
                "src/modules/s/s.c": "int plain;\n",
                "tools/shipped-js-allow.txt": "src/modules/k 50% bridge\nsrc/modules/s 7% gone port\nsrc/modules/z 9% removed\nsrc/modules/w 9%\n",
            })[p] ?? null,
        },
        miss: {
            files: ["src/modules/m/m.c", "src/modules/k/k.c", "src/core/x.c", "src/dyna-cli.c", "tools/shipped-js-allow.txt"],
            ignored: () => false,
            read: (p) => ({
                "src/modules/m/m.c": "static const char js[] = \"(1)\";\nv = JS_Eval(ctx, js, 3, \"<m>\", 0);\n" + "int pad;\n".repeat(20),
                "src/modules/k/k.c": "static const char kjs[] = \"(function () { return 1; })\";\nv = JS_Eval(ctx, kjs, 27, \"<k>\", 0);\n" + "int pad;\n".repeat(4),
                "src/core/x.c": "static const char big[] = \"(function () { return 1; })\";\nv = JS_Eval(ctx, big, 1, \"\", 0);\n",
                "src/dyna-cli.c": "static const char s[] = \"(function () { return 2; })\";\nv = JS_Eval(ctx, s, 1, \"\", 0);\n",
                "tools/shipped-js-allow.txt": "# module max% reason\nsrc/modules/k 21% bridge\n",
            })[p] ?? null,
        },
        fires: [/src\/modules\/m: ships [0-9.]+% JS source/, /src\/dyna-q: ships [0-9.]+% JS source/, /allow\.txt:1: src\/modules\/k ships [0-9.]+% JS; lower the entry from 50%/, /allow\.txt:2: src\/modules\/s ships 0% JS, within the 5% cap; delete/, /allow\.txt:3: src\/modules\/z does not exist/, /allow\.txt:4: entry needs "module max% reason"/],
    },
    {
        id: "dangling-test-ref",
        why: "a gate script that runs a test file by name must name a file that exists: when a suite is split or renamed, a stale path fails only on the leg that reaches it (the linux docker leg ran the pre-split test_es_conformance_fixes.js)",
        check: danglingTestRefRule,
        fire: {
            files: ["build.sh", "docker/build-and-test.sh", "tests/core/test_a.js"],
            ignored: () => false,
            read: (p) => ({
                "build.sh": "run tests/core/test_a.js\nrun tests/core/test_gone.js\n",
                "docker/build-and-test.sh": "dynajs tests/core/test_es_conformance_fixes.js\n",
            })[p] ?? null,
        },
        miss: {
            files: ["build.sh", "docker/build-and-test.sh", "tools/x.sh", "tests/core/test_a.js"],
            ignored: () => false,
            read: (p) => ({
                "build.sh": "run tests/core/test_a.js\n#   ./build.sh t tests/t.js\nfor f in tests/core/*_fixes.js; do :; done\n",
                "docker/build-and-test.sh": "for f in tests/core/test_core_*_es_conformance_fixes.js; do dynajs \"$f\"; done\n",
                "tools/x.sh": "ls my-tests/core/x.js\n",
            })[p] ?? null,
        },
        fires: [/build\.sh:2: tests\/core\/test_gone\.js does not exist/, /build-and-test\.sh:1: tests\/core\/test_es_conformance_fixes\.js does not exist/],
    },
    {
        id: "core-self-ref",
        why: "linked nodes (a struct holding pointers to its own type) cost a cache miss per hop and cannot be saved or mapped zero-copy; src/core structures use u32 indices into array pools, and every remaining linked struct is listed in tools/core-selfref-allow.txt with its reason, a list that only shrinks",
        check: selfRefRule,
        fire: {
            files: ["src/core/ds/a.c", "src/core/ds/b.h", "src/core/ds/b.c", "src/core/ds/c.c", "tools/core-selfref-allow.txt"],
            ignored: () => false,
            read: (p) => ({
                "src/core/ds/a.c": "typedef struct node {\n    struct node* next;\n    int v;\n} node_t;\n",
                "src/core/ds/b.h": "typedef struct tnode tnode_t;\n",
                "src/core/ds/b.c": "struct tnode {\n    double k;\n    tnode_t *l, *r;\n};\n",
                "src/core/ds/c.c": "typedef struct {\n    int x;\n} plain_t;\nstruct q {\n    const struct q* parent[2];\n};\n",
                "tools/core-selfref-allow.txt": "src/core/ds/c.c:q   \nsrc/core/ds/gone.c:old keeps a freelist\n",
            })[p] ?? null,
        },
        miss: {
            files: ["src/core/ds/a.c", "src/core/ds/b.c", "src/modules/x/x.c", "tools/core-selfref-allow.txt"],
            ignored: () => false,
            read: (p) => ({
                "src/core/ds/a.c": "typedef struct node {\n    uint32_t next, prev;\n    struct other* o;\n    int (*cmp)(struct node* a, struct node* b);\n} node_t;\n",
                "src/core/ds/b.c": "struct w {\n    struct w* retired;\n};\n",
                "src/modules/x/x.c": "struct m {\n    struct m* next;\n};\n",
                "tools/core-selfref-allow.txt": "# reasons are required\nsrc/core/ds/b.c:w lock-free reclamation list, not a container\n",
            })[p] ?? null,
        },
        fires: [/a\.c:1: struct node links to itself through next/, /b\.c:1: struct tnode links to itself through l, r/, /allow\.txt:1: entry needs/, /allow\.txt:2: src\/core\/ds\/gone\.c:old has no self-referential/, /c\.c:4: struct q links to itself through parent\[2\]/],
    },
    {
        id: "test-covers",
        why: "every suite names the ONE d.ts symbol it covers in its first 3 lines (// covers: dyna:csv.parse); the symbol must exist in dynajs.d.ts and be imported and used; several symbols only under integration|blackbox|example|contract with a -- reason; tools/covers-legacy.txt lists the suites not yet converted, and the count of suites without a header may never exceed COVERS_UNCOVERED_CAP (lower it as suites are split; it never rises)",
        check: coversRule,
        fire: {
            files: ["dynajs.d.ts", "tests/csv/test_csv_parse_rows.js", "tests/csv/test_csv_new.js", "tests/csv/test_csv_two.js", "tests/csv/test_csv_old.js", "tests/csv/test_csv_parse_noimp.js", "tests/csv/test_csv_csv_file_write.js", "tests/csv/test_csv_misnamed.js", "tests/csv/test_csv_wide.js", "tests/csv/test_csv_parse_unused.js", "tests/csv/test_csv_left.js", "tools/covers-legacy.txt"],
            coversCap: 0,
            ignored: () => false,
            read: (p) => ({
                "dynajs.d.ts": "\ndeclare module \"dyna:csv\" {\n    function parse(text: string): string[][];\n    class CSVFile {\n        read(): string[][];\n    }\n}\n",
                "tests/csv/test_csv_parse_rows.js": "// covers: dyna:csv.parseRows\nimport { parse } from \"dyna:csv\";\nparse(\"a\");\n",
                "tests/csv/test_csv_new.js": "import { parse } from \"dyna:csv\";\nparse(\"a\");\n",
                "tests/csv/test_csv_two.js": "// covers: dyna:csv.parse dyna:csv.CSVFile\nimport { parse, CSVFile } from \"dyna:csv\";\nparse(new CSVFile());\n",
                "tests/csv/test_csv_old.js": "// covers: dyna:csv.parse\nimport { parse } from \"dyna:csv\";\nparse(\"a\");\n",
                "tests/csv/test_csv_parse_noimp.js": "// covers: dyna:csv.parse\nparse(\"a\");\n",
                "tests/csv/test_csv_csv_file_write.js": "// covers: dyna:csv.CSVFile.write\nimport { CSVFile } from \"dyna:csv\";\nnew CSVFile().write();\n",
                "tests/csv/test_csv_misnamed.js": "// covers: dyna:csv.parse\nimport { parse } from \"dyna:csv\";\nparse(\"a\");\n",
                "tests/csv/test_csv_wide.js": "// covers: dyna:csv.*\nimport * as csv from \"dyna:csv\";\n",
                "tests/csv/test_csv_parse_unused.js": "// covers: dyna:csv.parse\nimport { parse as csvParse } from \"dyna:csv\";\nparseRows(\"a\");\n",
                "tests/csv/test_csv_left.js": "import { parse } from \"dyna:csv\";\nparse(\"a\");\n",
                "tools/covers-legacy.txt": "tests/csv/test_csv_old.js\ntests/csv/test_gone.js\ntests/csv/test_csv_left.js\n",
            })[p] ?? null,
        },
        miss: {
            files: ["dynajs.d.ts", "tests/csv/test_csv_parse.js", "tests/csv/test_csv_csv_file_read.js", "tests/blackbox/bb_csv.js", "tests/csv/test_csv_legacy.js", "tests/csv/test_csv_inputs.js", "tests/csv/test_csv_parse_alias.js", "tests/csv/test_csv_codec_run.js", "tests/lib/kit.js", "tools/covers-legacy.txt"],
            coversCap: 1,
            ignored: () => false,
            read: (p) => ({
                "dynajs.d.ts": "\ndeclare module \"dyna:csv\" {\n    function parse(text: string): string[][];\n    class CSVFile {\n        read(): string[][];\n    }\n    const Codec: {\n        run(text: string): void;\n    };\n}\n",
                "tests/csv/test_csv_codec_run.js": "// covers: dyna:csv.Codec.run\nimport { Codec } from \"dyna:csv\";\nCodec.run(\"a\");\n",
                "tests/csv/test_csv_parse.js": "// flags: --std\n// covers: dyna:csv.parse\nimport { parse } from \"dyna:csv\";\nparse(\"a\");\n",
                "tests/csv/test_csv_csv_file_read.js": "// covers: dyna:csv.CSVFile.read\nimport * as csv from \"dyna:csv\";\nnew csv.CSVFile().read();\n",
                "tests/blackbox/bb_csv.js": "// covers: blackbox dyna:csv.parse dyna:csv.CSVFile -- the module end to end through its public surface\nconst { parse, CSVFile } = await import(\"dyna:csv\");\nparse(new CSVFile());\n",
                "tests/csv/test_csv_legacy.js": "import { parse } from \"dyna:csv\";\n",
                "tests/csv/test_csv_inputs.js": "// flags: --std\n// covers: contract dyna:csv.* -- signature-driven sweep of every declared name\nawait fuzzModule(\"csv\");\n",
                "tests/csv/test_csv_parse_alias.js": "// covers: dyna:csv.parse\nimport { parse as csvParse } from \"dyna:csv\";\ncsvParse(\"a\");\n",
                "tests/lib/kit.js": "export const x = 1;\n",
                "tools/covers-legacy.txt": "# not yet converted\ntests/csv/test_csv_legacy.js\n",
            })[p] ?? null,
        },
        fires: [
            /test_csv_parse_rows\.js: covers dyna:csv\.parseRows: "dyna:csv" declares no parseRows/,
            /test_csv_new\.js: no "\/\/ covers:" header/,
            /test_csv_two\.js: covers 2 symbols/,
            /test_csv_old\.js now has a covers header/,
            /test_gone\.js no longer exists/,
            /test_csv_parse_noimp\.js: covers dyna:csv\.parse but never imports/,
            /test_csv_csv_file_write\.js: covers dyna:csv\.CSVFile\.write: .*declares no member write/,
            /test_csv_misnamed\.js: covers dyna:csv\.parse, so its name is test_csv_parse/,
            /test_csv_wide\.js: covers dyna:csv\.\* \(the whole module\) only under a category/,
            /test_csv_parse_unused\.js: covers dyna:csv\.parse but never uses parse/,
            /^1 suites have no covers header; that count only shrinks \(cap 0/,
        ],
    },
    {
        id: "markdown-tracked",
        why: "only README.md and API.md are committed; plans, notes and guides stay local",
        check: (repo) => repo.files.filter((p) => /\.md$/i.test(p) && p !== "README.md" && p !== "API.md"),
        fire: { files: ["README.md", "docs/plan.md"], ignored: () => true },
        miss: { files: ["README.md", "API.md", "src/x.c"], ignored: () => true },
    },
    {
        id: "test-data-ignored",
        why: "test data (corpora, reference vectors, fuzz seeds, expected tables) is git-ignored and listed in tests/test-data.tsv; tools/test-data.js fetch rebuilds it",
        check: (repo) => {
            const isData = (p) =>
                /^tests\/corpus\//.test(p) ||
                /^src\/fuzz\/corpus_[^/]+\//.test(p) ||
                /^tests\/.*(_vectors|\.expected)\.json$/.test(p);
            const out = repo.files.filter(isData).map((p) => `${p} is test data but tracked or not ignored`);
            const manifest = repo.read("tests/test-data.tsv");
            if (manifest === null) return out.concat(["tests/test-data.tsv is missing"]);
            for (const row of manifest.split("\n").slice(1)) {
                const [path, sha, src] = row.split("\t");
                if (!path) continue;
                if (!sha || !src) out.push(`tests/test-data.tsv: row ${path} needs path, sha256 and source`);
                else if (!repo.ignored(path)) out.push(`tests/test-data.tsv: ${path} is not git-ignored`);
            }
            return out;
        },
        fire: {
            files: ["tests/test-data.tsv", "tests/corpus/blns.txt", "src/fuzz/corpus_x/a.bin", "tests/crypto/x_vectors.json"],
            ignored: (p) => p !== "tests/corpus/new.txt",
            read: () => "path\tsha256\tsource\ntests/corpus/new.txt\tab\turl:x\ntests/corpus/bad.txt\tab\n",
        },
        fires: [/blns\.txt is test data/, /corpus_x\/a\.bin is test data/, /x_vectors\.json is test data/, /new\.txt is not git-ignored/, /bad\.txt needs path/],
        miss: {
            files: ["tests/test-data.tsv", "tests/corpus_test.js", "src/fuzz/fuzz_x.c", "tests/crypto/test_x.js"],
            ignored: () => true,
            read: () => "path\tsha256\tsource\ntests/corpus/blns.txt\tab\turl:x\n",
        },
    },
    {
        id: "tools-dynajs",
        why: "every file under tools/ is a dynajs script, never Python (the last two, split-tu.py and split-header.py, were ported on 2026-11-20 and the legacy list removed)",
        check: (repo) => repo.files
            .filter((p) => /^tools\/.*\.py$/.test(p) || p === "tools/py-legacy.txt")
            .map((p) => (p.endsWith(".py") ? `${p} is Python under tools/: write it as a dynajs script` : `${p} is back: there is no legacy Python list any more`)),
        fire: {
            files: ["tools/a.py", "tools/sub/b.py", "tools/py-legacy.txt", "tools/x.js"],
            ignored: () => false,
            read: () => "",
        },
        fires: [/tools\/a\.py is Python/, /tools\/sub\/b\.py is Python/, /py-legacy\.txt is back/],
        miss: {
            files: ["tools/x.js", "tools/split-tu.js", "tests/ref/gen.py", "bench/b.py"],
            ignored: () => false,
            read: () => "",
        },
    },
    {
        id: "py-exec-frozen",
        why: "NO-PYTHON: a test or script may not start python3; a wire peer is a dynajs process on tests/lib/rawpeer.js (proved by tools/peer-proof.js). Files still doing it are listed in tools/py-exec-baseline.txt, which only shrinks; the reference-oracle runners (tests/ref, tools/test-data.js) are exempt",
        check: (repo) => {
            const base = (repo.read("tools/py-exec-baseline.txt") || "").split("\n").filter((l) => l.length > 0);
            const rx = /\bpython3?\s+(?:\$\{|[\w./${}-]*\.py\b|-[cmu]\b)|["'`]python3?["'`]\s*[,\]]|\bWhich\(\s*["']python3?["']\s*\)|command -v python3?\b/;
            const exempt = /^(tests\/ref\/|bench\/|third_party\/|tools\/(test-data|repo-rules|sweep|peer-proof)\.js$)|\.(py|md|txt|tsv)$/;
            const hit = repo.files.filter((p) => !exempt.test(p) && /\.(js|sh|c|mk)$|(^|\/)Dockerfile/.test(p) && rx.test(repo.read(p) || ""));
            const out = hit.filter((p) => !base.includes(p)).map((p) => `${p} starts python3: write the peer as a dynajs script on tests/lib/rawpeer.js`);
            for (const l of base) if (!hit.includes(l)) out.push(`tools/py-exec-baseline.txt lists ${l}, which no longer starts python3: drop the line`);
            if (base.length > 45) out.push(`tools/py-exec-baseline.txt has ${base.length} entries; it may only shrink (45 on 2026-11-28)`);
            return out;
        },
        fire: {
            files: ["tools/py-exec-baseline.txt", "tests/a/test_new.js", "tests/b/run.sh", "tests/c/test_old.js", "tests/d/test_gate.js"],
            ignored: () => false,
            read: (p) => ({ "tools/py-exec-baseline.txt": "tests/c/test_old.js\ntests/gone.js\n" + Array.from({ length: 45 }, (_, i) => "tests/pad" + i + ".js").join("\n"),
                "tests/a/test_new.js": "sh(`python3" + " ${T}/evil.py ${T}/evil.port`);", "tests/b/run.sh": "python3" + " -c 'print(1)'",
                "tests/c/test_old.js": "os.exec([\"python3\", T + \"/cli.py\"]);", "tests/d/test_gate.js": "if (!Which(\"python3\")) skip();" })[p] || "",
        },
        fires: [/test_new\.js starts python3/, /run\.sh starts python3/, /test_gate\.js starts python3/, /lists tests\/gone\.js/, /may only shrink/, ...Array.from({ length: 45 }, () => /lists tests\/pad/)],
        miss: {
            files: ["tools/py-exec-baseline.txt", "tests/c/test_old.js", "tests/ref/py.sh", "tools/test-data.js", "tests/e/test_text.js", "tests/f/x.md"],
            ignored: () => false,
            read: (p) => ({ "tools/py-exec-baseline.txt": "tests/c/test_old.js\n", "tests/c/test_old.js": "sh(`python3" + " ${T}/m.py`);",
                "tests/ref/py.sh": "python3" + " gen.py", "tools/test-data.js": "python3" + " -m pip", "tests/e/test_text.js": "// vs the python oracle; u.v5(NS, \"python.org\")",
                "tests/f/x.md": "python3" + " x.py" })[p] || "",
        },
    },
    {
        id: "py-outside-tools",
        why: "Python outside tools/ is frozen: the reference-oracle generators under tests/ref (numpy/scipy/networkx/sklearn), the pty drivers and tools_agent are listed in tools/py-outside-tools.txt, which only shrinks; new scripts are dynajs",
        check: (repo) => {
            const legacy = (repo.read("tools/py-outside-tools.txt") || "").split("\n").filter((l) => l.length > 0);
            const out = repo.files
                .filter((p) => /\.py$/.test(p) && !/^(tools|third_party)\//.test(p) && !legacy.includes(p))
                .map((p) => `${p} is Python outside tools/ and not in tools/py-outside-tools.txt: write it as a dynajs script`);
            for (const l of legacy) if (!repo.files.includes(l)) out.push(`tools/py-outside-tools.txt lists ${l}, which is gone: drop the line`);
            if (legacy.length > 23) out.push(`tools/py-outside-tools.txt has ${legacy.length} entries; it may only shrink (23 on 2026-11-20: 20 + the 4 bench numpy/scipy/networkx baselines - keys_driver.py, ported)`);
            return out;
        },
        fire: {
            files: ["tools/py-outside-tools.txt", "tests/ref/gen_a.py", "tests/x/new.py", "tools/y.py"],
            ignored: () => false,
            read: () => "tests/ref/gen_a.py\ntests/gone.py\n" + Array.from({ length: 22 }, (_, i) => `tests/ref/pad${i}.py`).join("\n"),
        },
        fires: [/tests\/x\/new\.py is Python outside/, /lists tests\/gone\.py/, /may only shrink/, ...Array.from({ length: 22 }, () => /lists tests\/ref\/pad/)],
        miss: {
            files: ["tools/py-outside-tools.txt", "tests/ref/gen_a.py", "tools/y.py", "third_party/z.py", "tests/x/ok.js"],
            ignored: () => false,
            read: () => "tests/ref/gen_a.py\n",
        },
    },
    {
        id: "tests-named-cases",
        why: "every suite has named cases (tests/lib/kit.js suite() or sections() -- a sloppy script loads it with import() --, or inputSweep() from tests/lib/sigfuzz.js, one kit case per callable) so one case can be listed and run alone; flat suites are legacy listed in tests/flat-suites.txt, which only shrinks",
        check: (repo) => {
            const legacy = (repo.read("tests/flat-suites.txt") || "").split("\n").filter((l) => l.length > 0);
            const isSuite = (p) => /^tests\/[^/]+\/test_[^/]*\.js$/.test(p) && !/^tests\/(agent|lib)\//.test(p);
            const kit = /(?:from |import\()["'](\.\.\/)+(lib\/)?kit\.js["']|\binputSweep\b[^\n]*from ["'](\.\.\/)+lib\/sigfuzz\.js["']/;
            const out = repo.files
                .filter((p) => isSuite(p) && !legacy.includes(p) && !kit.test(repo.read(p) || ""))
                .map((p) => `${p} has no named cases: build it with suite() or sections() from tests/lib/kit.js`);
            for (const l of legacy) {
                if (!repo.files.includes(l)) out.push(`tests/flat-suites.txt lists ${l}, which is gone: drop the line`);
                else if (kit.test(repo.read(l) || "")) out.push(`tests/flat-suites.txt lists ${l}, which now has named cases: drop the line`);
            }
            if (legacy.length > 77) out.push(`tests/flat-suites.txt has ${legacy.length} entries; it may only shrink (77 on 2026-11-20)`);
            return out;
        },
        fire: {
            files: ["tests/flat-suites.txt", "tests/a/test_old.js", "tests/a/test_new.js", "tests/a/test_conv.js", "tests/a/test_kit.js"],
            ignored: () => false,
            read: (p) => ({
                "tests/flat-suites.txt": "tests/a/test_old.js\ntests/a/test_conv.js\ntests/a/test_gone.js\n",
                "tests/a/test_old.js": "print(1);\n",
                "tests/a/test_new.js": "print(2);\n",
                "tests/a/test_conv.js": "import { sections } from \"../lib/kit.js\";\n",
                "tests/a/test_kit.js": "import { suite } from \"../lib/kit.js\";\n",
            })[p],
        },
        fires: [/test_new\.js has no named cases/, /lists tests\/a\/test_gone\.js, which is gone/, /lists tests\/a\/test_conv\.js, which now has named cases/],
        miss: {
            files: ["tests/flat-suites.txt", "tests/a/test_old.js", "tests/a/test_kit.js", "tests/a/test_sweep.js", "tests/a/test_script.js", "tests/lib/test_helper.js", "tests/agent/test_x.js", "tests/test_top.sh"],
            ignored: () => false,
            read: (p) => ({
                "tests/flat-suites.txt": "tests/a/test_old.js\n",
                "tests/a/test_old.js": "print(1);\n",
                "tests/a/test_kit.js": "import { suite } from \"../lib/kit.js\";\n",
                "tests/a/test_sweep.js": "import { inputSweep } from \"../lib/sigfuzz.js\";\n",
                "tests/a/test_script.js": "import(\"../lib/kit.js\").then(async ({ sections }) => {});\n",
                "tests/lib/test_helper.js": "print(3);\n",
                "tests/agent/test_x.js": "print(4);\n",
            })[p],
        },
    },
    {
        id: "tests-no-cwd-scratch",
        why: "a suite that writes a fixed file name into the working directory (the repo root) races every concurrent run of itself or a sibling (cg_test_upgrade.env: 4 of 20 paired runs failed); scratch files go under TMPDIR with a unique name. Path.cwd().join(NAME) may only name a tracked file (reading README.md is fine)",
        check: (repo) => {
            const out = [];
            const have = new Set(repo.files);
            for (const p of repo.files) {
                if (!/^tests\/.*\.js$/.test(p) || /^tests\/lib\//.test(p)) continue;
                const text = repo.read(p) || "";
                for (const m of text.matchAll(/Path\.cwd\(\)\.join\("([^"]+)"\)/g)) {
                    const name = m[1].replace(/^(\.\.?\/)+/, "");
                    if (!have.has(name)) out.push(`${p}: Path.cwd().join("${m[1]}") names no tracked file -- put scratch files under TMPDIR with a unique name`);
                }
            }
            return out;
        },
        fire: {
            files: ["tests/a/test_x.js", "README.md"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_x.js": `const f = Path.cwd().join("scratch.env");\nconst r = Path.cwd().join("README.md");\n` })[p],
        },
        fires: [/test_x\.js: Path\.cwd\(\)\.join\("scratch\.env"\) names no tracked file/],
        miss: {
            files: ["tests/a/test_x.js", "README.md", "tests/lib/helper.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_x.js": `const r = Path.cwd().join("README.md");\nconst t = new Path(tmp + "/x.env");\n`, "tests/lib/helper.js": `Path.cwd().join("anything.tmp");\n` })[p],
        },
    },
    {
        id: "tests-perf-gated",
        why: "the docker linux legs check compatibility, not speed (CLAUDE.md rule 13): an emulated or shared container skews cost ratios (M1a-08 measured 16 ns vs 82 ns per cell there, green on the host). A relative-cost assertion (x < y * k, k * x < y) in a suite that times things goes through perfCheck from tests/lib/perf.js, which is a SKIP under DYNA_COMPAT_ONLY=1; a comparison that is not about speed says `not perf:` on its line",
        check: (repo) => {
            const out = [];
            const timing = /performance\.now\(|\bcpuNow\b|\bcpuMs\b|\bbestMs\b|\bcpuClock\b|hrtime/;
            const assertion = /\b(ok|assert|check|expect)\s*\(|\bif\s*\(\s*!\s*\(/;
            const relcost = /[\w.)\]]\s*<=?\s*[\w.(]+\s*\*\s*[\w.]+|[\w.)\]]\s*\*\s*[\w.]+\s*<=?\s*[\w.(]/;
            for (const p of repo.files) {
                if (!/^tests\/(?!lib\/|agent\/)(.*\/)?test_[^/]*\.js$/.test(p)) continue;
                const text = repo.read(p) || "";
                if (!timing.test(text)) continue;
                text.split("\n").forEach((line, i) => {
                    if (/^\s*\/\//.test(line) || !assertion.test(line) || !relcost.test(line)) return;
                    if (line.includes("not perf:")) return;
                    out.push(`${p}:${i + 1}: a relative-cost assertion in a timing suite -- wrap it in perfCheck(ok, cond, what) from tests/lib/perf.js, or say \`not perf: <why>\` on the line`);
                });
            }
            return out;
        },
        fire: {
            files: ["tests/a/test_x.js"],
            ignored: () => false,
            read: (p) => ({
                "tests/a/test_x.js": "const t = performance.now();\nok(wide < narrow * 4, \"flat cost\");\nok(tGood * 2 < tBad, \"skips\");\nif (!(a < b * 4)) fails++;\n",
            })[p],
        },
        fires: [/test_x\.js:2: a relative-cost assertion/, /test_x\.js:3: a relative-cost assertion/, /test_x\.js:4: a relative-cost assertion/],
        miss: {
            files: ["tests/a/test_x.js", "tests/a/test_nt.js", "tests/lib/test_h.js", "tests/a/bench_x.js"],
            ignored: () => false,
            read: (p) => ({
                "tests/a/test_x.js": "const t = cpuNow();\nperfCheck(ok, wide < narrow * 4, \"flat cost\");\nok(hi.length < lo.length * 0.95, \"smaller\"); // not perf: output size\n// ok(a < b * 4)\nfor (let i = 0; i < n * 2; i++) {}\n",
                "tests/a/test_nt.js": "ok(c < n * 2, \"count bound\");\n",
                "tests/lib/test_h.js": "performance.now(); ok(a < b * 4, \"x\");\n",
                "tests/a/bench_x.js": "performance.now(); ok(a < b * 4, \"x\");\n",
            })[p],
        },
    },
    {
        id: "tests-spawn-running-binary",
        why: "a suite that starts dynajs as a child must start the binary that runs the suite (args()[0] from dyna:sys); cwd() + \"/dynajs\" tests ./dynajs instead, so a run with any other build (--out .obj-X, a snapshot) silently checks old code (test_http_client_sync_keepalive failed on .obj-ties while ./dynajs predated the patch, 2026-11-16)",
        check: (repo) => {
            const out = [];
            for (const p of repo.files) {
                if (!/^tests\/(?!lib\/|agent\/).*\.js$/.test(p)) continue;
                (repo.read(p) || "").split("\n").forEach((line, i) => {
                    if (/^\s*\/\//.test(line)) return;
                    if (/\bcwd\(\)\s*\+\s*["'`]\/dynajs["'`]/.test(line)) out.push(`${p}:${i + 1}: starts ./dynajs as a child -- use args()[0] from dyna:sys so the child is the binary running this suite`);
                });
            }
            return out;
        },
        fire: {
            files: ["tests/a/test_x.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_x.js": "const c = new Spawn(cwd() + \"/dynajs\", [\"--std\", T]);\nExec(cwd() + '/dynajs', []);\n" })[p],
        },
        fires: [/test_x\.js:1: starts \.\/dynajs as a child/, /test_x\.js:2: starts \.\/dynajs as a child/],
        miss: {
            files: ["tests/a/test_x.js", "tests/lib/h.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_x.js": "const c = new Spawn(args()[0], [\"--std\", T]);\n// was cwd() + \"/dynajs\"\nconst d = cwd() + \"/data\";\n", "tests/lib/h.js": "Spawn(cwd() + \"/dynajs\");\n" })[p],
        },
    },
    {
        id: "tests-helper-slots",
        why: "a suite that keeps a helper process running beside itself (spawnHelper: a mock server, a child dynajs, a fake peer) must declare \"// slots: N\" (N >= 2) in its first 10 lines: tools/run-tests-parallel.sh then takes N machine-wide slots (tools/test-slot.sh -n N), so DYN_TEST_CAP bounds processes, not suites; with one slot the leaks leg read cap+1..+3 processes (F-cap-1, g56 2026-11-19)",
        check: (repo) => {
            const out = [];
            for (const p of repo.files) {
                if (!SUITE(p)) continue;
                const text = repo.read(p) || "";
                const code = stripJsComments(text);
                if (!/\bspawnHelper\s*\(/.test(code)) continue;
                const m = /^\/\/[ \t]*slots:[ \t]*(\d+)/m.exec(text.split("\n").slice(0, 10).join("\n"));
                if (!m || +m[1] < 2) out.push(`${p}: starts a helper with spawnHelper but declares ${m ? "slots: " + m[1] : "no \"// slots: N\""} -- add \"// slots: 2\" to its first 10 lines`);
            }
            return out;
        },
        fire: {
            files: ["tests/a/test_x.js", "tests/a/test_y.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_x.js": "// flags: --std\nconst h = spawnHelper(cmd);\n", "tests/a/test_y.js": "// slots: 1\nspawnHelper(cmd);\n" })[p],
        },
        fires: [/test_x\.js: starts a helper with spawnHelper but declares no/, /test_y\.js: starts a helper with spawnHelper but declares slots: 1/],
        miss: {
            files: ["tests/a/test_x.js", "tests/a/test_z.js", "tests/lib/helper.js", "tests/a/w_child.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_x.js": "// flags: --std\n// slots: 2\nspawnHelper(cmd);\n", "tests/a/test_z.js": "// spawnHelper(cmd) was here\nok(1);\n", "tests/lib/helper.js": "export function spawnHelper(c) {}\n", "tests/a/w_child.js": "spawnHelper(cmd);\n" })[p],
        },
    },
    {
        id: "bench-solo",
        why: "a suite whose file name says bench or perf measures time, CPU or RSS, which a parallel wave turns into noise (test_dkim_bench failed in a run2 wave and passed solo in 1 s, 2026-11-28): it declares \"// solo: REASON\" in its first 10 lines, which tools/run-tests-parallel.sh and run2.sh read through SOLO_RE",
        check: (repo) => {
            const out = [];
            for (const p of repo.files) {
                if (!SUITE(p) || !/(bench|perf)[^/]*$/.test(p)) continue;
                const head = (repo.read(p) || "").split("\n").slice(0, 10).join("\n");
                if (!/^\/\/[ \t]*solo:[ \t]*\S/m.test(head)) out.push(`${p}: a bench/perf suite without \"// solo: REASON\" in its first 10 lines runs in the parallel wave -- add the line`);
            }
            return out;
        },
        fire: {
            files: ["tests/a/test_a_bench.js", "tests/a/test_perf_x.js", "tests/a/bb_bench.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_a_bench.js": "// flags: --std\nok(1);\n", "tests/a/test_perf_x.js": "// flags: --std\n// solo:\n", "tests/a/bb_bench.js": "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n// solo: late\n" })[p],
        },
        fires: [/test_a_bench\.js: a bench\/perf suite without/, /test_perf_x\.js: a bench\/perf suite without/, /bb_bench\.js: a bench\/perf suite without/],
        miss: {
            files: ["tests/a/test_a_bench.js", "tests/a/test_plain.js", "tests/bench/bench_x.js", "tests/a/test_a_bench_child.js"],
            ignored: () => false,
            read: (p) => ({ "tests/a/test_a_bench.js": "// flags: --std\n// solo: timing\n", "tests/a/test_plain.js": "ok(1);\n", "tests/bench/bench_x.js": "ok(1);\n", "tests/a/test_a_bench_child.js": "ok(1);\n" })[p],
        },
    },
    {
        id: "tests-module-dir",
        why: "every module split is also a test split: a suite under a module's tests dir (tools/modules.tsv, tests column) names that module (dyna:<m> in an import or a child script), or carries an integration|blackbox|contract covers header with its reason; and a tests/net suite naming a module split out of dyna:net (http dns proxy netip metrics postgres redis sqlite) must be such an integration suite, so the net row's tests prove net alone (B1e)",
        check: (repo) => {
            const out = [];
            const tsv = repo.read("tools/modules.tsv");
            if (tsv === null) return out;
            const owner = new Map();
            for (const line of tsv.split("\n")) {
                const c = line.split("\t");
                if (c.length < 8 || c[0] === "module" || line.startsWith("#")) continue;
                const exp = c[2].split(/\s+/).filter((x) => /^dyna:[\w-]+$/.test(x)).map((x) => x.slice(5));
                if (!exp.length) continue;
                for (const d of c[7].split(/\s+/).filter(Boolean)) owner.set(d.replace(/\/+$/, ""), { mod: c[0], exp, src: (c[3] + " " + c[4]).split(/\s+/).filter(Boolean) });
            }
            const NET_SPLIT = ["http", "dns", "proxy", "netip", "metrics", "postgres", "redis", "sqlite"];
            for (const p of repo.files) {
                if (!SUITE(p)) continue;
                const o = owner.get(p.slice(0, p.lastIndexOf("/")));
                if (!o) continue;
                const text = repo.read(p) || "";
                const head = coversHeader(text) || "";
                if (/^(integration|blackbox|contract)\s/.test(head.trim())) continue;
                const own = (head.match(/(?:^|\s)c:(\S+)/g) || []).map((x) => x.trim().slice(2));
                if (own.length && own.every((f) => kitIn(o.src, f))) continue;
                const names = new Set([...stripJsComments(text).matchAll(/dyna:([\w-]+)\\?["'`]/g)].map((x) => x[1]));
                if (!o.exp.some((m) => names.has(m))) out.push(`${p}: a suite under ${p.slice(0, p.lastIndexOf("/"))}/ names none of ${o.exp.map((m) => "dyna:" + m).join(", ")} -- move it to the dir of the module it tests, or give it an integration covers header with its reason`);
                if (o.mod === "net") {
                    const hit = NET_SPLIT.filter((m) => names.has(m));
                    if (hit.length) out.push(`${p}: a tests/net suite names ${hit.map((m) => "dyna:" + m).join(", ")}, split out of dyna:net -- move the case to that module's tests dir, or make it an integration suite with its reason`);
                }
            }
            return out;
        },
        fire: {
            files: ["tools/modules.tsv", "tests/net/test_net_db.js", "tests/csv/test_csv_parse.js"],
            ignored: () => false,
            read: (p) => ({
                "tools/modules.tsv": "module\tkind\texports\tsrc\tkits\tpkg\tdeps\ttests\tdescription\nnet\tmodule\tdyna:net\tsrc/dyna-net.c\t\t\t\ttests/net\tsockets\ncsv\tmodule\tdyna:csv\tsrc/csv.c\t\t\t\ttests/csv\tcsv\n",
                "tests/net/test_net_db.js": "// covers: dyna:net.TCPServer\nimport { TCPServer } from \"dyna:net\";\nimport { Redis } from \"dyna:redis\";\nnew Redis({ port: new TCPServer().port });\n",
                "tests/csv/test_csv_parse.js": "// covers: dyna:json.parse\nimport { parse } from \"dyna:json\";\nparse(\"1\");\n",
            })[p] ?? null,
        },
        fires: [/test_net_db\.js: a tests\/net suite names dyna:redis/, /test_csv_parse\.js: a suite under tests\/csv\/ names none of dyna:csv/],
        miss: {
            files: ["tools/modules.tsv", "tests/net/test_net_tcp.js", "tests/net/test_net_interop.js", "tests/csv/test_csv_child.js", "tests/csv/test_csv_native.js", "tests/lib/kit.js"],
            ignored: () => false,
            read: (p) => ({
                "tools/modules.tsv": "module\tkind\texports\tsrc\tkits\tpkg\tdeps\ttests\tdescription\nnet\tmodule\tdyna:net\tsrc/dyna-net.c\t\t\t\ttests/net\tsockets\ncsv\tmodule\tdyna:csv\tsrc/csv.c\t\t\t\ttests/csv\tcsv\n",
                "tests/net/test_net_tcp.js": "// covers: dyna:net.TCPServer\nimport { TCPServer } from \"dyna:net\";\n// dyna:redis was once tested here\nnew TCPServer();\n",
                "tests/net/test_net_interop.js": "// covers: integration dyna:net.TCPServer dyna:http.App -- one loop shared by both\nimport { TCPServer } from \"dyna:net\";\nimport { App } from \"dyna:http\";\n",
                "tests/csv/test_csv_child.js": "// covers: c:src/csv.c\nwriteFile(\"c.js\", 'import { parse } from \"dyna:csv\"; parse(\"a\");');\n",
                "tests/csv/test_csv_native.js": "// covers: c:src/csv.c\nimport { suite } from \"../lib/kit.js\";\n",
                "tests/lib/kit.js": "import { Redis } from \"dyna:redis\";\n",
            })[p] ?? null,
        },
    },
    {
        id: "local-only-ignored",
        why: "local notes, agent workspaces and fuzzer reproducers must be git-ignored before any tool writes them",
        check: (repo) =>
            [
                "CLAUDE.md",
                ".agent-work/x",
                "docs/x.md",
                "crash-0",
                "leak-0",
                "timeout-0",
                "oom-0",
                "slow-unit-0",
            ].filter((p) => !repo.ignored(p)),
        fire: { files: [], ignored: (p) => p !== "crash-0" },
        miss: { files: [], ignored: () => true },
    },
];

function capture(args) {
    const [r, w] = os.pipe();
    const pid = os.exec(args, { stdout: w, blocking: false });
    os.close(w);
    const f = std.fdopen(r, "r");
    const out = f.readAsString();
    f.close();
    const [, status] = os.waitpid(pid, 0);
    if (status !== 0) throw new Error(args.join(" ") + " exited " + status);
    return out;
}

function realRepo() {
    const listed = capture(["git", "ls-files", "--cached", "--others", "--exclude-standard"])
        .split("\n")
        .filter((p) => p.length > 0);
    const files = listed.filter((p) => {
        const [st, err] = os.stat(p);
        return err === 0 && (st.mode & os.S_IFMT) === os.S_IFREG;
    });
    const ignoredCache = new Map();
    const ignored = (p) => {
        if (!ignoredCache.has(p)) {
            const pid = os.exec(["git", "check-ignore", "-q", "--no-index", p], { blocking: false });
            const [, status] = os.waitpid(pid, 0);
            ignoredCache.set(p, status === 0);
        }
        return ignoredCache.get(p);
    };
    return { files, ignored, read: (p) => std.loadFile(p) };
}

function scan(repo) {
    const found = [];
    for (const rule of REPO_RULES) for (const what of rule.check(repo)) found.push(`${rule.id}: ${what}: ${rule.why}`);
    for (const p of repo.files) {
        const rules = FILE_RULES.filter((r) => r.scope(p));
        if (!rules.length) continue;
        const text = repo.read(p);
        if (text === null) continue;
        for (const rule of rules) for (const line of rule.check(text)) found.push(`${rule.id}: ${p}:${line}: ${rule.why}`);
    }
    return found;
}

function selftest() {
    let bad = 0;
    const samplePath = {
        "ere-word-boundary": "tools/x.sh",
        "pipe-masks-status": "tools/x.sh",
        "subst-before-status": "build.sh",
        "pipefail-grep-q": "build.sh",
        "exclude-list": "docker/x.sh",
        "if-const": "src/x.c",
        "tracked-alloc-member": "src/core/dyn-ds.h",
        "tracked-ab-take": "src/modules/graph/graph-module.c",
        "time-only-tmp-name": "tests/file/test_x.js",
        "std-import-needs-flag": "tests/probes/probe_x.js",
        "cpuclock-result-object": "tests/structures/test_x.js",
        "persist-path-type": "dynajs.d.ts",
        "discarded-arm": "src/dyna-aio-uring.c",
        "struct-hack": "src/libregexp.c",
        "strict-c17-consts": "src/core/dyn-ds.c",
        "doc-tense": "dynajs.d.ts",
        "async-twin": "dynajs.d.ts",
        "test-fixed-port": "tests/http/test_x.js",
        "test-wall-clock-ratio": "tests/core/test_x.js",
        "test-wall-clock-budget": "tests/core/test_x.js",
        "test-background-helper": "tests/http/test_x.js",
        "fixed-tmp-path": "tools/check-x.py",
        "ncpu-default": "build.sh",
        "unsafe-libc": "src/core/dyn-x.c",
        "host-slot-pool": "build.sh",
    };
    for (const rule of FILE_RULES) {
        const p = samplePath[rule.id];
        if (!p || !rule.scope(p)) {
            print(`selftest: ${rule.id} has no in-scope sample path`);
            bad++;
            continue;
        }
        const n = rule.check(rule.fire).length;
        if (n === 0) {
            print(`selftest: ${rule.id} did NOT fire on its plant`);
            bad++;
        } else if (rule.fireCount !== undefined && n !== rule.fireCount) {
            print(`selftest: ${rule.id} fired ${n} times on its plants, expected ${rule.fireCount}`);
            bad++;
        }
        if (rule.check(rule.miss).length !== 0) {
            print(`selftest: ${rule.id} fired on its near miss`);
            bad++;
        }
    }
    for (const rule of REPO_RULES) {
        const fired = rule.check(rule.fire);
        if (fired.length === 0) {
            print(`selftest: ${rule.id} did NOT fire on its plant`);
            bad++;
        } else if (rule.fires !== undefined) {
            for (const want of rule.fires) {
                if (!fired.some((f) => want.test(f))) {
                    print(`selftest: ${rule.id} did not report ${want} on its plants; it said:\n  ${fired.join("\n  ")}`);
                    bad++;
                }
            }
            if (fired.length !== rule.fires.length) {
                print(`selftest: ${rule.id} reported ${fired.length} findings on its plants, expected exactly ${rule.fires.length}:\n  ${fired.join("\n  ")}`);
                bad++;
            }
        }
        if (rule.check(rule.miss).length !== 0) {
            print(`selftest: ${rule.id} fired on its near miss`);
            bad++;
        }
    }
    const n = FILE_RULES.length + REPO_RULES.length;
    print(bad ? `repo-rules selftest: ${bad} problem(s) in ${n} rules` : `repo-rules selftest: ${n} rules fire on their plants, none on their near misses`);
    return bad === 0;
}

const mode = scriptArgs[1];
if (mode === "selftest") {
    std.exit(selftest() ? 0 : 1);
} else if (mode === undefined) {
    const found = scan(realRepo());
    for (const f of found) print(f);
    print(found.length ? `repo-rules: ${found.length} violation(s)` : `repo-rules: clean (${FILE_RULES.length + REPO_RULES.length} rules)`);
    std.exit(found.length ? 1 : 0);
} else {
    print("usage: dynajs --std tools/repo-rules.js [selftest]");
    std.exit(2);
}
