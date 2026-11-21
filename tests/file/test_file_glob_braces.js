// covers: dyna:file.glob
// Brace expansion in glob(), Glob and Watcher ignore. Before it existed,
// glob("src/**/*.{c,h}") returned [] with no error (dogfood G13). Oracles:
// bash itself for the expansion (set -f, so only braces act), `find` for the
// walk, and glob() of each bash-expanded alternative for the merged result.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_glob_braces");
import { glob, Glob, Watcher, Path, writeFile, makeDir, makeTempDir, removeAll } from "dyna:file";
import { Exec } from "dyna:sys";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { const x = JSON.stringify(a), y = JSON.stringify(b); ok(x === y, what + " (got " + x + ", want " + y + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

const root = makeTempDir("globbr");
const R = String(root);
const files = ["src/a.c", "src/a.h", "src/b.txt", "src/sub/c.c", "src/sub/d.h", "src/sub/e.cc", "src/.hid.c",
    "tests/t.c", "tests/u.h", "x", "x.bak", "{lit}.txt", "{a,b}", "a{", "q1", "q2"];
for (const f of files) {
    const i = f.lastIndexOf("/");
    if (i > 0) makeDir(new Path(R + "/" + f.slice(0, i)), { recursive: true });
    writeFile(new Path(R + "/" + f), f);
}
const g = (pat) => glob(pat, { cwd: root }).map(String);

function bashExpand(pat) {
    const r = Exec("/bin/bash", ["-c", 'set -f; eval "set -- $1"; printf "%s\\n" "$@"', "_", pat]);
    if (r.code !== 0) throw new Error("bash failed on " + pat + ": " + r.stderr);
    return String(r.stdout).split("\n").slice(0, -1);
}
const ORACLE = ["src/**/*.{c,h}", "{src,tests}/*.c", "src/{a,sub/{c,d}}.{c,h}", "x{,.bak}", "{src,tests}/{a,t,u}.{c,h,txt}",
    "src/{a,b}*", "{x,nope,q?}", "src/sub/*.{cc,h}", "{tests,src/sub}/[cdt].c"];
for (const pat of ORACLE) {
    const alts = bashExpand(pat);
    ok(alts.length > 1, pat + ": bash expands it (" + alts.length + ")");
    const union = [...new Set(alts.flatMap((a) => g(a)))].sort();
    const got = g(pat);
    eq(got, union, pat + ": glob == union of glob() over bash's expansion");
    ok(got.length > 0, pat + ": finds something");
    const gl = new Glob(pat);
    ok(gl.hasWildcard, pat + ": hasWildcard");
    for (const a of alts) ok(gl.matches(new Path(a.replace(/[*?]/g, "z").replace(/\[([a-z])[a-z]*\]/g, "$1"))) || /\*\*/.test(a), pat + ": Glob matches a concrete instance of alternative " + a);
}

__kitS.section("src/**/*.{c,h} equals find's answer (dotfiles skipped)", () => {
    const fr = Exec("/usr/bin/find", ["src", "(", "-name", "*.c", "-o", "-name", "*.h", ")", "-not", "-name", ".*"], { cwd: R });
    const want = String(fr.stdout).split("\n").filter(Boolean).sort();
    eq(g("src/**/*.{c,h}"), want, "src/**/*.{c,h} equals find's answer (dotfiles skipped)");
    eq(want.length, 4, "find oracle sanity: a.c a.h sub/c.c sub/d.h");
});

eq(g("{lit}.txt"), ["{lit}.txt"], "a group without a top-level comma is literal");
eq(g("[{]a,b}"), ["{a,b}"], "a brace inside [..] is literal: [{]a,b} names the file {a,b}");
eq(g("a{"), ["a{"], "an unbalanced { is literal");
eq(g("q{1..2}"), [], "{1..2} sequences are not expanded (literal)");
eq(g("x{,}"), ["x"], "duplicate alternatives collapse in the sorted, de-duplicated result");
eq(g("{x,x.bak,x}"), ["x", "x.bak"], "repeated alternative appears once");
eq(g("{x,{x,x.bak}}"), ["x", "x.bak"], "nested groups");
eq(g("{}"), [], "an empty {} is literal and matches nothing here");

__kitS.section("exactly 4096 alternatives is allowed (2^12)", () => {
    const k12 = "{a,b}".repeat(12), k13 = "{a,b}".repeat(13);
    eq(g(k12), [], "exactly 4096 alternatives is allowed (2^12)");
    eq(err(() => g(k13)), "RangeError: glob: brace expansion yields more than 4096 patterns", "8192 alternatives throw RangeError");
    eq(err(() => new Glob(k13)), "RangeError: Glob: brace expansion yields more than 4096 patterns", "the Glob constructor refuses too");
    eq(err(() => Glob.match("a", k13)), "RangeError: Glob.match: brace expansion yields more than 4096 patterns", "Glob.match refuses too");
    eq(new Glob(k12).matches(new Path("ab".repeat(6))), true, "the 4096th-boundary Glob still matches");
    ok(err(() => new Watcher(root, { ignore: ["ok", k13] })).startsWith("RangeError: Watcher: ignore[1] brace expansion"), "Watcher ignore refuses past the cap");
});

__kitS.section("Glob.match expands braces", () => {
    eq(Glob.match("lib/x.ts", "**/*.{js,ts}"), true, "Glob.match expands braces");
    eq(Glob.match("lib/x.py", "**/*.{js,ts}"), false, "and rejects the others");
    eq(Glob.match("{a,b}", "[{]a,b}"), true, "bracketed brace stays literal in Glob.match");
    eq(Glob.match("{x}", "{x}"), true, "{x} is literal in Glob.match");
    const gl = new Glob("*.{c,h}");
    eq(gl.filter(["a.c", "a.h", "a.cc", "b.txt"].map((s) => new Path(s))).map(String), ["a.c", "a.h"], "Glob.filter uses every alternative");
    eq(gl.pattern, "*.{c,h}", "pattern keeps the source text");
    eq(new Glob("{x}").hasWildcard, false, "a literal group is not a wildcard");
    eq(new Glob("{x,y}").hasWildcard, true, "an expanding group is a wildcard");
    eq(gl.expand(new Path(R + "/src")).map(String), ["a.c", "a.h"], "Glob.expand walks every alternative");
});

__kitS.section("Watcher ignore expands braces: only c.txt is tracked", () => {
    const wd = makeTempDir("globbrw");
    for (const f of ["a.tmp", "b.log", "c.txt"]) writeFile(new Path(String(wd) + "/" + f), f);
    const w = new Watcher(wd, { ignore: ["*.{tmp,log}"] });
    w.start(() => {});
    eq(w.stats().entries, 1, "Watcher ignore expands braces: only c.txt is tracked");
    w.close();
    removeAll(wd);
});
__kitS.end();

removeAll(root);
print("test_file_glob_braces: " + n + " checks, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
