// covers: dyna:file.Glob
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_glob_handle");
import {
    Path, File, Glob, readFile, writeFile, stat, exists, remove,
    makeTempDir, removeAll, makeDir, glob, realPath,
} from "dyna:file";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

__kitS.section("the pattern is readable back", () => {
const root = makeTempDir("dyna-file-handle-");
let ok = false;
try {

{
    makeDir(root.join("g"));
    makeDir(root.join("g", "sub"));
    writeFile(root.join("g", "one.txt"), "1");
    writeFile(root.join("g", "two.txt"), "2");
    writeFile(root.join("g", "top.js"), "3");
    writeFile(root.join("g", "sub", "deep.js"), "4");
    const base = root.join("g");

    const gl = new Glob("*.txt");
    eq(gl.pattern, "*.txt", "the pattern is readable back");
    eq(gl.hasWildcard, true, "and the wildcard flag is computed once, at construction");
    eq(new Glob("exact.txt").hasWildcard, false, "a literal pattern has none");

    eq(gl.matches(new Path("one.txt")), true, "matches a name");
    eq(gl.matches(new Path("one.js")), false, "rejects another");
    eq(gl.matches(new Path("does-not-exist-anywhere.txt")), true,
        "matches is lexical: it never touches the disk");

    const viaClass = gl.expand(base).map(String).sort();
    const viaFree = glob("*.txt", { cwd: base }).map(String).sort();
    eq(viaClass.join(","), viaFree.join(","), "expand equals the free glob()");
    eq(viaClass.join(","), "one.txt,two.txt", "and finds the right files");

    const filtered = gl.filter([new Path("a.txt"), new Path("b.js"), new Path("c.txt")]);
    eq(filtered.map(String).join(","), "a.txt,c.txt", "filter keeps the matches");
    eq(filtered.every(Path.isPath), true, "and yields Paths");
    eq(gl.filter([]).length, 0, "an empty list filters to empty");

    eq(new Glob("**/*.js").expand(base).map(String).sort().join(","),
        "sub/deep.js,top.js", "** spans zero or more directories");
    eq(new Glob("*.js").expand(base).map(String).join(","), "top.js",
        "* does not cross a separator");

    throwsWith(() => new Glob(), "requires a string", "the pattern is required");
    throwsWith(() => new Glob(42), "requires a string", "a non-string pattern is refused");
    eq(gl.matches("one.txt"), gl.matches(new Path("one.txt")), "T5: matches takes a string path like a Path");
    throwsWith(() => gl.matches(42), "must be a Path", "matches refuses a number");
    throwsWith(() => gl.filter("nope"), "requires an array", "filter wants an array");
}

{
    const paths = [];
    for (let i = 0; i < 200; i++) paths.push(new Path("file" + i + (i % 3 ? ".txt" : ".js")));
    const gl = new Glob("*.txt");

    const t0 = performance.now();
    for (let r = 0; r < 50; r++) gl.filter(paths);
    const hoisted = performance.now() - t0;

    const t1 = performance.now();
    for (let r = 0; r < 50; r++) new Glob("*.txt").filter(paths);
    const perCall = performance.now() - t1;

    print("  Glob hoisted " + hoisted.toFixed(2) + " ms vs per-call " +
          perCall.toFixed(2) + " ms over 200 paths x 50 -> " +
          (perCall / hoisted).toFixed(2) + "x");
}

{
    const edge = new Glob("*b");
    for (const sub of ["", "a", "ab", "ba", "abab", "aaaaaaaaaaaaaaaaaaaa"])
        edge.matches(new Path(sub));
    const edge2 = new Glob("*x*yz");
    for (const sub of ["", "a", "ax", "ayz", "aaaaaaaaaaaaaaaaaaaa"])
        edge2.matches(new Path(sub));
    assert(!edge.matches(new Path("a")) && !edge2.matches(new Path("a")),
           "star-backtrack edge shapes still match nothing");
}

ok = true;
} finally {
    removeAll(root);
}
assert(ok, "the suite ran to completion");
assert(!exists(root), "the temp tree is cleaned up");
});
__kitS.end();

print("test_file_handle: all " + n + " assertions passed");
