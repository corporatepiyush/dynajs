// covers: integration dyna:file.readFile dyna:file.writeFile dyna:file.stat dyna:file.lstat dyna:file.copyFile dyna:file.File -- symlink-follow policy checked across every path-taking call on one fixture tree
// dyna:file opens and stats user paths with the platform's own resolution: a symlink anywhere in the path -- the
// final component, an intermediate directory (absolute or relative link) -- is followed. The old opener walked the path one component at a time with O_NOFOLLOW and refused all of these (ELOOP).
// removeAll and glob still never descend through a symlinked directory; that is pinned elsewhere.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_symlink_follow", { chain: true });
import { Path, makeTempDir, writeFile, readFile, stat, lstat, copyFile, removeAll, makeDir, symlink, realPath, File,
         exists } from "dyna:file";

let failures = 0, passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else { failures++; print("FAIL " + msg); }
}
function attempt(fn) {
    try { return { v: fn() }; } catch (e) { return { e: String(e) }; }
}

const dir = makeTempDir("symfollow");
const real = new Path(dir, "real");
makeDir(real);
writeFile(new Path(real, "a.txt"), "alpha");
symlink(String(new Path(real, "a.txt")), new Path(dir, "leaf.ln"));
symlink(String(real), new Path(dir, "dir.ln"));
symlink("real", new Path(dir, "reldir.ln"));

let r = attempt(() => readFile(new Path(dir, "leaf.ln")));
ok(r.v === "alpha", "readFile through a symlinked final component: " + JSON.stringify(r));
r = attempt(() => readFile(new Path(dir, "dir.ln", "a.txt")));
ok(r.v === "alpha", "readFile through a symlinked intermediate directory: " + JSON.stringify(r));
r = attempt(() => readFile(new Path(dir, "reldir.ln", "a.txt")));
ok(r.v === "alpha", "readFile through a relative symlinked directory: " + JSON.stringify(r));
r = attempt(() => readFile(new Path(dir, "leaf.ln"), { bytes: true }));
ok(r.v && r.v.length === 5, "readFile bytes through a symlink: " + JSON.stringify(r.e));

__kitS.section("writeFile through a symlinked directory lands in the target:", () => {
r = attempt(() => writeFile(new Path(dir, "dir.ln", "b.txt"), "beta"));
ok(!r.e && readFile(new Path(real, "b.txt")) === "beta", "writeFile through a symlinked directory lands in the target: " + r.e);
r = attempt(() => writeFile(new Path(dir, "leaf.ln"), "gamma"));
ok(!r.e && readFile(new Path(real, "a.txt")) === "gamma", "writeFile through a symlinked leaf writes the target: " + r.e);
ok(lstat(new Path(dir, "leaf.ln")).isSymlink, "the link itself is still a link after a write through it");
});

__kitS.section("stat through a symlinked directory:", () => {
r = attempt(() => stat(new Path(dir, "dir.ln", "a.txt")));
ok(r.v && r.v.isFile && r.v.size === 5, "stat through a symlinked directory: " + JSON.stringify(r.e));
r = attempt(() => lstat(new Path(dir, "dir.ln", "a.txt")));
ok(r.v && r.v.isFile && !r.v.isSymlink, "lstat of a file under a symlinked directory: " + JSON.stringify(r.e));
r = attempt(() => lstat(new Path(dir, "leaf.ln")));
ok(r.v && r.v.isSymlink, "lstat of a symlinked leaf describes the link");
});

__kitS.section("copyFile from a symlinked source:", () => {
r = attempt(() => { copyFile(new Path(dir, "leaf.ln"), new Path(dir, "copy.txt")); return readFile(new Path(dir, "copy.txt")); });
ok(r.v === "gamma", "copyFile from a symlinked source: " + JSON.stringify(r));
r = attempt(() => lstat(new Path(dir, "copy.txt")));
ok(r.v && !r.v.isSymlink, "copyFile writes a regular file, not a link");
});

__kitS.section("File opened through a symlinked directory:", () => {
r = attempt(() => new File(new Path(dir, "dir.ln", "a.txt")).readText());
ok(r.v === "gamma", "File opened through a symlinked directory: " + JSON.stringify(r));
});

__kitS.section("symlink follow failed", () => {
removeAll(dir);
ok(!exists(dir), "cleanup");
print(`file symlink follow: ${passed} passed, ${failures} failed`);
if (failures) throw new Error("symlink follow failed");
});
__kitS.end();
