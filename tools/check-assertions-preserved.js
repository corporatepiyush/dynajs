// flags: --std
// Refuse a test restructuring that loses an assertion.
//
// Splitting a multi-symbol suite into per-symbol files (and merging scattered
// same-symbol fragments) moves assertion call sites between files. A move is
// fine; a DROP is not. This compares, per test module (tests/<module>/ and the
// module a blackbox file names), the MULTISET of normalized assertion call
// sites before and after:
//
//   every call site present before must be present after, at least as often.
//
// A call site is the full text of a call to an assertion-shaped helper
// (assert*, check*, expect*, eq*, ok, same*, must*, *_eq, case_eq, throws*,
// verify*, deepEqual ...), whitespace-collapsed, comments stripped. Adding
// assertions is free; rewording one counts as a drop of the old text plus an
// addition, and is refused unless named in --allow (a file of call texts that
// are known rewrites, one per line), so a rewording is a decision, not drift.
//
//   check-assertions-preserved.js --before DIR --after DIR [--module M ...] [--allow FILE]
//   check-assertions-preserved.js --before-rev REV [--after DIR] [--module M ...]
//       (REV is read with `git show REV:path` in the repo at --repo, default .)
//   check-assertions-preserved.js count FILE...      per-file call-site counts
//   check-assertions-preserved.js selftest           a planted drop must be refused
//
// Exit 0 when nothing was dropped, 1 with the dropped call sites listed.
import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";

const W = "[\\p{L}\\p{N}_]";
const WS = "[\\t\\n\\x0b\\x0c\\r\\x1c-\\x1f \\x85\\xa0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000]";
const WS_RUN = new RegExp(WS + "+", "gu");
const WS_EDGE = new RegExp("^" + WS + "+|" + WS + "+$", "gu");
const FUNC_TAIL = new RegExp("function" + WS + "*$", "u");
const norm = (t) => t.replace(WS_RUN, " ").replace(WS_EDGE, "");
const CALL = new RegExp(
    `(?<![\\p{L}\\p{N}_$.])((?:assert${W}*|check(?:_${W}+|[A-Z]${W}*)?|expect${W}*|eq${W}*|ok|same${W}*|must${W}*|` +
    `${W}+_eq|case_eq|throws${W}*|verify${W}*|deepEqual|strictEqual|notEqual|equal)${WS}*\\()`, "gu");
const REGEX_AFTER = new Set("(,=:[!&|?{};+-*%<>~^");
const ALNUM = /^[\p{L}\p{N}]$/u;
const out = (s) => std.out.puts(s + "\n");

function regexEnd(src, i) {
    let j = i + 1, cls = false;
    const n = src.length;
    while (j < n && src[j] !== "\n") {
        if (src[j] === "\\") { j += 2; continue; }
        if (src[j] === "[") cls = true;
        else if (src[j] === "]") cls = false;
        else if (src[j] === "/" && !cls) return j + 1;
        j += 1;
    }
    return -1;
}

const wordCh = (c) => ALNUM.test(c) || c === "_" || c === "$";

function prevSig(src, i) {
    let j = i - 1;
    while (j >= 0 && " \t\r\n".includes(src[j])) j--;
    if (j < 0) return "";
    if (wordCh(src[j])) {
        let k = j;
        while (k >= 0 && wordCh(src[k])) k--;
        const word = src.slice(k + 1, j + 1);
        return ["return", "typeof", "case", "in", "of", "void", "throw", "new", "delete"].includes(word) ? "(" : "w";
    }
    return src[j];
}

function isRegex(src, i) {
    if (src.startsWith("//", i) || src.startsWith("/*", i)) return false;
    const p = prevSig(src, i);
    return p === "" || REGEX_AFTER.has(p);
}

function stripComments(src) {
    const o = [];
    let i = 0;
    const n = src.length;
    while (i < n) {
        const c = src[i];
        if (c === "/" && isRegex(src, i)) {
            const e = regexEnd(src, i);
            if (e > 0) { o.push(src.slice(i, e)); i = e; continue; }
        }
        if (c === '"' || c === "'" || c === "`") {
            let j = i + 1;
            while (j < n && src[j] !== c) j += src[j] === "\\" ? 2 : 1;
            o.push(src.slice(i, j + 1));
            i = j + 1;
        } else if (src.startsWith("//", i)) {
            const j = src.indexOf("\n", i);
            i = j === -1 ? n : j;
        } else if (src.startsWith("/*", i)) {
            const j = src.indexOf("*/", i + 2);
            i = j === -1 ? n : j + 2;
        } else {
            o.push(c);
            i += 1;
        }
    }
    return o.join("");
}

export function callSites(src) {
    const code = stripComments(src);
    const sites = [];
    for (const m of code.matchAll(CALL)) {
        const start = m.index, end = m.index + m[0].length;
        if (FUNC_TAIL.test(code.slice(Math.max(0, start - 12), start))) continue;
        let depth = 0, j = end - 1;
        while (j < code.length) {
            const ch = code[j];
            if (ch === "/" && isRegex(code, j)) {
                const e = regexEnd(code, j);
                if (e > 0) { j = e; continue; }
            }
            if (ch === '"' || ch === "'" || ch === "`") {
                j += 1;
                while (j < code.length && code[j] !== ch) j += code[j] === "\\" ? 2 : 1;
            } else if ("([{".includes(ch)) depth += 1;
            else if (")]}".includes(ch)) {
                depth -= 1;
                if (depth === 0) break;
            }
            j += 1;
        }
        sites.push(norm(code.slice(start, j + 1)));
    }
    return sites;
}

export function moduleOf(path) {
    let m = /^(?:.*\/)?tests\/blackbox\/bb_([a-z0-9_]+)\.js$/.exec(path);
    if (m) return "blackbox:" + m[1];
    m = /^(?:.*\/)?tests\/([^/]+)\//.exec(path);
    return m ? m[1] : "?";
}

function isDir(p) {
    const [st, e] = os.stat(p);
    return e === 0 && (st.mode & os.S_IFMT) === os.S_IFDIR;
}

function treeFiles(root) {
    const res = [];
    const walk = (d) => {
        const [names, e] = os.readdir(d);
        if (e) return;
        const dirs = [];
        for (const n of names) {
            if (n === "." || n === "..") continue;
            const full = d + "/" + n;
            if (isDir(full)) dirs.push(full);
            else if (n.endsWith(".js")) res.push([full.slice(root.replace(/\/+$/, "").length + 1), full]);
        }
        for (const s of dirs) walk(s);
    };
    walk(root.replace(/\/+$/, "") + "/tests");
    return res;
}

class Tally {
    constructor() { this.per = new Map(); this.where = new Map(); }
    add(mod, s, rel) {
        if (!this.per.has(mod)) { this.per.set(mod, new Map()); this.where.set(mod, new Map()); }
        const c = this.per.get(mod);
        c.set(s, (c.get(s) || 0) + 1);
        if (!this.where.get(mod).has(s)) this.where.get(mod).set(s, rel);
    }
    total() {
        let n = 0;
        for (const c of this.per.values()) for (const v of c.values()) n += v;
        return n;
    }
}

function collectDir(root, modules) {
    const t = new Tally();
    for (const [rel, full] of treeFiles(root)) {
        const mod = moduleOf(rel);
        if (modules.size && !modules.has(mod)) continue;
        const src = std.loadFile(full);
        if (src === null) continue;
        for (const s of callSites(src)) t.add(mod, s, rel);
    }
    return t;
}

function collectRev(repo, rev, modules) {
    const t = new Tally();
    const r = Exec("git", ["ls-tree", "-r", "--name-only", rev, "--", "tests"], { cwd: repo, maxBuffer: 1 << 30 });
    if (r.code !== 0) {
        std.err.puts(String(r.stderr));
        std.exit(1);
    }
    for (const rel of String(r.stdout).split(/\s+/).filter(Boolean)) {
        if (!rel.endsWith(".js")) continue;
        const mod = moduleOf(rel);
        if (modules.size && !modules.has(mod)) continue;
        const src = String(Exec("git", ["show", `${rev}:${rel}`], { cwd: repo, maxBuffer: 1 << 30 }).stdout);
        for (const s of callSites(src)) t.add(mod, s, rel);
    }
    return t;
}

const byKey = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

function compare(before, after, allow) {
    const dropped = [];
    for (const mod of [...before.per.keys()].sort(byKey)) {
        const aft = after.per.get(mod) || new Map();
        for (const [s, n] of before.per.get(mod)) {
            const m = aft.get(s) || 0;
            if (m < n && !allow.has(s)) dropped.push([mod, before.where.get(mod).get(s) || "?", s, n, m]);
        }
    }
    return dropped;
}

export function selftest() {
    const a = { "tests/csv/test_csv.js": 'import {assert_eq} from "x";\nassert_eq(parse("a"), 1); // c\nassert_eq(parse("b"),\n   2);\ncheck("x" === y, "msg");\n' };
    const bOk = { "tests/csv/test_csv_parse.js": 'assert_eq(parse("a"), 1);\nassert_eq(parse("b"), 2);\n',
        "tests/csv/test_csv_check.js": 'check("x" === y, "msg"); assert(true);\n' };
    const bBad = { "tests/csv/test_csv_parse.js": 'assert_eq(parse("a"), 1);\ncheck("x" === y, "msg");\n' };
    const per = (files) => {
        const t = new Tally();
        for (const [rel, src] of Object.entries(files)) for (const s of callSites(src)) t.add(moduleOf(rel), s, rel);
        return t;
    };
    const pa = per(a);
    let bad = 0;
    const csv = pa.per.get("csv") || new Map();
    if ([...csv.values()].reduce((x, y) => x + y, 0) !== 3) {
        out(`selftest: expected 3 call sites in the plant, got ${JSON.stringify(Object.fromEntries(csv))}`);
        bad++;
    }
    if (compare(pa, per(bOk), new Set()).length) {
        out("selftest: a pure MOVE (split into two files) was refused");
        bad++;
    }
    const d = compare(pa, per(bBad), new Set());
    if (d.length !== 1 || !d[0][2].includes('parse("b")')) {
        out(`selftest: the planted DROP was not refused exactly once: ${JSON.stringify(d)}`);
        bad++;
    }
    const rx = callSites('assert(/^\\{"a":"[^"]+"\\}$/.test(x), "m");\nassert(y, "n");\n');
    if (JSON.stringify(rx) !== JSON.stringify(['assert(/^\\{"a":"[^"]+"\\}$/.test(x), "m")', 'assert(y, "n")'])) {
        out(`selftest: a quote inside a regex literal swallowed the next call site: ${JSON.stringify(rx)}`);
        bad++;
    }
    const cm = callSites("assert_eq(x, // the note\n  1);\n");
    if (JSON.stringify(cm) !== JSON.stringify(["assert_eq(x, 1)"])) {
        out(`selftest: a comment inside a call stayed in its call site: ${JSON.stringify(cm)}`);
        bad++;
    }
    const rq = callSites('const q = /"/; // assert(old)\nassert(y, 1);\n');
    if (JSON.stringify(rq) !== JSON.stringify(["assert(y, 1)"])) {
        out(`selftest: a quote in a regex literal hid the comment after it from stripping: ${JSON.stringify(rq)}`);
        bad++;
    }
    if (callSites("function assert_eq(a, b) { if (a !== b) throw 1; }\n").length) {
        out("selftest: a helper DEFINITION counted as a call site");
        bad++;
    }
    out("check-assertions-preserved selftest: " + (!bad ? "ok" : `${bad} problem(s)`));
    return bad ? 1 : 0;
}

const USAGE = "usage: check-assertions-preserved.js [--before BEFORE] [--before-rev BEFORE_REV] [--repo REPO] [--after AFTER] [--module MODULE] [--allow ALLOW]";

function usageError(msg) {
    std.err.puts(`${USAGE}\ncheck-assertions-preserved.js: error: ${msg}\n`);
    std.exit(2);
}

export function main(argv) {
    if (argv[0] === "selftest") return selftest();
    if (argv[0] === "count") {
        for (const f of argv.slice(1)) out(`${String(callSites(std.loadFile(f) || "").length).padStart(6)}  ${f}`);
        return 0;
    }
    const o = { before: null, beforeRev: null, repo: ".", after: ".", module: [], allow: null };
    const keys = { "--before": "before", "--before-rev": "beforeRev", "--repo": "repo", "--after": "after", "--module": "module", "--allow": "allow" };
    for (let i = 0; i < argv.length; i++) {
        let a = argv[i], v;
        const eq = a.indexOf("=");
        if (a.startsWith("--") && eq > 0) { v = a.slice(eq + 1); a = a.slice(0, eq); }
        const k = keys[a];
        if (!k) usageError(`unrecognized arguments: ${argv.slice(i).join(" ")}`);
        if (v === undefined) {
            if (i + 1 >= argv.length) usageError(`argument ${a}: expected one argument`);
            v = argv[++i];
        }
        if (k === "module") o.module.push(v);
        else o[k] = v;
    }
    const mods = new Set(o.module);
    let before;
    if (o.beforeRev) before = collectRev(o.repo, o.beforeRev, mods);
    else if (o.before) before = collectDir(o.before, mods);
    else usageError("--before DIR or --before-rev REV is required");
    const after = collectDir(o.after, mods);
    const allow = new Set();
    if (o.allow) {
        const t = std.loadFile(o.allow);
        if (t === null) { std.err.puts(`check-assertions-preserved: cannot read ${o.allow}\n`); return 1; }
        for (const l of t.split(/(?<=\n)/)) if (norm(l)) allow.add(norm(l));
    }
    const dropped = compare(before, after, allow);
    const nb = before.total(), na = after.total();
    for (const [mod, src, s, n, m] of dropped) out(`  DROPPED [${mod}] ${src}: ${s}   (before x${n}, after x${m})`);
    const scope = mods.size ? [...mods].sort(byKey).join(",") : "all modules";
    if (dropped.length) {
        out(`check-assertions-preserved: FAIL ${dropped.length} assertion call site(s) dropped (${scope}; ${nb} before, ${na} after)`);
        return 1;
    }
    out(`check-assertions-preserved: ok (${scope}; ${nb} call sites before, ${na} after, none dropped)`);
    return 0;
}

if (typeof scriptArgs !== "undefined" && /check-assertions-preserved\.js$/.test(scriptArgs[0] || ""))
    std.exit(main(scriptArgs.slice(1)));
