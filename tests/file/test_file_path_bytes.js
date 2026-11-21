// flags: --std
// covers: dyna:file.Path
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_path_bytes");
import * as std from "std";
import {
    Path, File, FileReader, readFile, readBytes, writeFile, makeDir,
    removeAll, remove, glob, Glob, FileLock, Watcher, symlink,
} from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; throw new Error("assertion failed: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL(no throw): " + m); }
    catch (e) {
        if (!re.test(String(e.message))) { fails++; print(`FAIL(${e.message}): ` + m); }
    }
}

const dir = new Path(Path.temp(), "dj_fbytes." + (Date.now() % 10000000) + "_" + Math.random().toString(36).slice(2, 10));
makeDir(dir, { recursive: true });
const bin = new Path(dir, "bin.dat");

function fresh(data, bufsize) {
    writeFile(bin, data);
    return new FileReader(bin, { bufferSize: bufsize || 4 });
}
function eq(a, b, m) {
    assert(a.length === b.length && a.every((v, i) => v === b[i]),
           m + ` (${JSON.stringify(Array.from(a))} vs ${JSON.stringify(Array.from(b))})`);
}

__kitS.section("** reaches nested files", () => {
    const gdir = new Path(dir, "gl");
    makeDir(new Path(gdir, "sub", "deep"), { recursive: true });
    makeDir(new Path(gdir, ".hiddendir"), { recursive: true });
    writeFile(new Path(gdir, "a.txt"), "x");
    writeFile(new Path(gdir, "b.txt"), "x");
    writeFile(new Path(gdir, ".hidden"), "x");
    writeFile(new Path(gdir, "sub", "c.txt"), "x");
    writeFile(new Path(gdir, "sub", "deep", "d.txt"), "x");
    writeFile(new Path(gdir, ".hiddendir", "e.txt"), "x");

    const names = (arr) => arr.map((p) => String(p)).map((s) => s.split("/").pop()).sort();

    const all = names(glob(new Path(gdir, "**")));
    assert(all.includes("a.txt") && all.includes("d.txt"), "** reaches nested files");
    assert(all.includes("sub") && all.includes("deep"), "a trailing ** also emits directories");
    assert(!all.includes(".hidden") && !all.includes("e.txt"), "** skips dotfile entries");
    const zero = names(glob(new Path(gdir, "sub", "**", "*.txt")));
    assert(zero.includes("c.txt") && zero.includes("d.txt"), "** between segments matches zero segments too");
    const one = names(glob(new Path(gdir, "*.txt")));
    assert(one.join(",") === "a.txt,b.txt", "* stays within one segment");
    assert(!one.includes(".hidden"), "* never matches a leading dot (minimatch rule)");
    assert(names(glob(new Path(gdir, ".*"))).includes(".hidden"), "a dot-leading pattern reaches dotfiles");
    assert(names(glob(new Path(gdir, "[ab].txt"))).join(",") === "a.txt,b.txt", "[ab] class");
    assert(names(glob(new Path(gdir, "[!a].txt"))).join(",") === "b.txt", "[!...] negated class");
    assert(names(glob(new Path(gdir, "?.txt"))).length === 2, "? is one character");
    assert(names(glob(new Path(gdir, "{a,b}.txt"))).join(",") === "a.txt,b.txt", "brace alternation expands (was silently [] before; see test_file_glob_braces)");
    assert(glob(new Path(gdir, "**"), { cwd: undefined }).length > 0, "cwd: undefined = no option");
    const cwded = glob("**", { cwd: gdir });
    assert(cwded.every((p) => !String(p).startsWith("/")), "{cwd} results are relative");
    assert(names(cwded).length === all.length - 1, "{cwd} walk = same tree minus the implied root");
    throws(() => glob("**", { followSymlinks: true }), /unknown option "followSymlinks" \(valid: cwd\)/,
           "followSymlinks does not exist and is refused, not silently ignored");

    const g = new Glob("**/*.txt");
    assert(g.matches(new Path(gdir, "sub", "c.txt")), "lexical ** spans");
    assert(g.matches(new Path("nowhere/at/all/x.txt")), "lexical match needs no filesystem");
    assert(g.matches(new Path(gdir, "a.txt")), "lexical **/ matches zero segments");
    const star = new Glob("*.txt");
    assert(star.matches(new Path(gdir, "sub", "c.txt")), "LEXICAL * crosses / (the walk's per-segment rule is the walk's)");
    assert(new Glob("*idden").matches(new Path(".hidden")), "lexical has NO leading-dot rule (walk-only), pinned");
    assert(star.filter([new Path(gdir, "a.txt"), new Path("z.json")]).length === 1, "filter uses the same lexical matcher");
    assert(new Glob("plain").hasWildcard === false, "hasWildcard");
    assert(g.matches("a/string") === g.matches(new Path("a/string")), "T5: matches takes a string path like a Path");
    throws(() => g.matches(7), /must be a Path/, "matches refuses a number");
    throws(() => new Glob(42), /string/, "Glob(pattern) requires a string");

    try {
        symlink(String(new Path(gdir, "sub")), new Path(gdir, "link"));
        const fulls = glob(new Path(gdir, "**")).map(String);
        assert(fulls.some((s) => s.split("/").pop() === "link"), "the symlink is emitted as a leaf");
        assert(!fulls.some((s) => s.slice(String(gdir).length + 1).startsWith("link/")),
               "** never walks THROUGH a symlinked dir");
    } catch (e) {
        print("  (glob symlink pin skipped: " + e.message + ")");
    }

    removeAll(gdir);
});
__kitS.end();

removeAll(dir);
print(`test_file_bytes: ${n} assertions passed`);
if (fails) throw new Error(fails + " failures");
