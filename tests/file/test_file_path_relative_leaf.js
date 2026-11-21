// flags: --std
// covers: dyna:file.Path
// A relative path with no slash ("notes.txt") resolves against the current directory. The old component-walking opener handed back
// AT_FDCWD for it, which is negative on every platform (-2 macOS, -100 Linux); its callers used to treat any negative
// value as failure, so every single-component relative path read as ENOENT while "sub/notes.txt" and absolute paths
// worked. Each entry point is driven with a bare leaf name from inside a scratch directory, beside the same call
// with a two-component path as the control.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_path_relative_leaf");
import { Path, makeTempDir, writeFile, readFile, readBytes, stat, lstat, exists, copyFile, rename, remove, removeAll,
         makeDir, readDir, sniffType, File } from "dyna:file";
import * as os from "os";

let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        print("FAIL " + msg);
    }
}
function attempt(fn) {
    try {
        return { v: fn() };
    } catch (e) {
        return { e: String(e) };
    }
}

__kitS.section("chdir into the scratch directory", () => {
const home = os.getcwd()[0];
const dir = makeTempDir("relleaf");
ok(os.chdir(String(dir)) === 0, "chdir into the scratch directory");
try {
    makeDir(new Path("sub"));
    for (const name of ["notes.txt", "sub/notes.txt"]) {
        const p = new Path(name);
        const w = attempt(() => writeFile(p, "hello leaf"));
        ok(w.v === 10, `${name}: writeFile writes 10 bytes (${w.e || w.v})`);
        const r = attempt(() => readFile(p));
        ok(r.v === "hello leaf", `${name}: readFile reads it back (${r.e || r.v})`);
        const b = attempt(() => readBytes(p));
        ok(b.v && b.v.length === 10, `${name}: readBytes (${b.e || b.v && b.v.length})`);
        ok(attempt(() => exists(p)).v === true, `${name}: exists`);
        const s = attempt(() => stat(p));
        ok(s.v && s.v.size === 10, `${name}: stat size (${s.e || s.v && s.v.size})`);
        const l = attempt(() => lstat(p));
        ok(l.v && l.v.size === 10, `${name}: lstat size (${l.e})`);
        const a = attempt(() => writeFile(p, "+", { append: true }));
        ok(a.v === 1 && attempt(() => readFile(p)).v === "hello leaf+", `${name}: append (${a.e})`);
        const st = attempt(() => sniffType(p));
        ok(typeof st.v === "string", `${name}: sniffType (${st.e})`);
        const f = attempt(() => new File(name).readText());
        ok(f.v === "hello leaf+", `${name}: File(string).readText (${f.e || f.v})`);
        const copy = new Path(name + ".copy");
        const c = attempt(() => copyFile(p, copy));
        ok(c.v === 11 && attempt(() => readFile(copy)).v === "hello leaf+", `${name}: copyFile (${c.e || c.v})`);
        const moved = new Path(name + ".moved");
        ok(!attempt(() => rename(copy, moved)).e && exists(moved) && !exists(copy), `${name}: rename`);
        ok(!attempt(() => remove(moved)).e && !exists(moved), `${name}: remove`);
        ok(attempt(() => exists(new Path(name + ".absent"))).v === false, `${name}: exists is false for a missing leaf`);
    }
    const listing = attempt(() => readDir(new Path(".")).map((e) => e.name).sort());
    ok(listing.v && listing.v.includes("notes.txt") && listing.v.includes("sub"), `readDir(".") (${listing.e})`);
    ok(attempt(() => stat(new Path("."))).v?.isDir === true, `stat(".") is a directory`);
} finally {
    os.chdir(home);
    removeAll(dir);
}
print(`file relative leaf: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
});
__kitS.end();
