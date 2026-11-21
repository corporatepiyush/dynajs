// covers: dyna:file.writeFile
// writeFile(path, data, {atomic: true}) (dogfood G6): temp file beside the
// target + fsync + rename. Oracles: the inode changes (rename replaced the
// file; a plain write keeps the inode), a pre-existing hard link (made with
// ln) still shows the OLD bytes, the mode carries over, a symlink keeps being
// a link to the updated target, and no temporary is ever left behind. The
// EACCES checks skip (SKIP line) when a probe write into the 0555 directory
// succeeds: the linux gate leg runs as root in docker, where modes do not bind.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_write_file_atomic");
import { Path, writeFile, readFile, stat, lstat, readDir, symlink, readLink, chmod, makeTempDir, removeAll } from "dyna:file";
import { Exec } from "dyna:sys";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { const x = JSON.stringify(a), y = JSON.stringify(b); ok(x === y, what + " (got " + x + ", want " + y + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + (e.code || e.message); } }

const dir = makeTempDir("atomicw");
const D = String(dir);
const P = (name) => new Path(D + "/" + name);
const names = () => readDir(dir).map((e) => e.name).sort();

__kitS.section("returns the byte count", () => {
    eq(writeFile(P("new.txt"), "hello", { atomic: true }), 5, "returns the byte count");
    eq(String(readFile(P("new.txt"))), "hello", "creates a new file with the content");
});
__kitS.section("a new atomic file gets the same mode as a plain writeFile (umask applied)", () => {
    writeFile(P("plain.txt"), "plain");
    const m = stat(P("plain.txt")).mode & 0o777;
    writeFile(P("fresh.txt"), "x", { atomic: true });
    eq(stat(P("fresh.txt")).mode & 0o777, m, "a new atomic file gets the same mode as a plain writeFile (umask applied)");
});
__kitS.section("old content", () => {
    writeFile(P("a.txt"), "old content");
    const ino0 = stat(P("a.txt")).ino;
    eq(Exec("/bin/ln", [D + "/a.txt", D + "/a-hardlink.txt"]).code, 0, "ln made a hard link to the old file");
    writeFile(P("a.txt"), "new content!", { atomic: true });
    eq(String(readFile(P("a.txt"))), "new content!", "the target has the new bytes");
    ok(stat(P("a.txt")).ino !== ino0, "the target is a NEW inode (renamed into place)");
    eq(String(readFile(P("a-hardlink.txt"))), "old content", "the old inode (hard link) still holds the old bytes: never modified in place");
    writeFile(P("b.txt"), "one");
    const inoB = stat(P("b.txt")).ino;
    writeFile(P("b.txt"), "two");
    eq(stat(P("b.txt")).ino, inoB, "control: a plain writeFile keeps the inode (so the inode oracle discriminates)");
});
__kitS.section("an existing 0600 file stays 0600", () => {
    writeFile(P("secret.txt"), "s");
    chmod(P("secret.txt"), 0o600);
    writeFile(P("secret.txt"), "s2", { atomic: true });
    eq(stat(P("secret.txt")).mode & 0o7777, 0o600, "an existing 0600 file stays 0600");
    chmod(P("secret.txt"), 0o751);
    writeFile(P("secret.txt"), "s3", { atomic: true });
    eq(stat(P("secret.txt")).mode & 0o7777, 0o751, "an existing 0751 file stays 0751");
});
__kitS.section("via link", () => {
    writeFile(P("target.txt"), "t0");
    symlink("target.txt", P("link.txt"));
    writeFile(P("link.txt"), "via link", { atomic: true });
    ok(lstat(P("link.txt")).isSymlink, "the symlink is still a symlink");
    eq(readLink(P("link.txt")), "target.txt", "pointing at the same target");
    eq(String(readFile(P("target.txt"))), "via link", "and the target got the new bytes");
});
__kitS.section("a 3 MiB write returns its length", () => {
    const bytes = new Uint8Array(3 << 20);
    for (let i = 0; i < bytes.length; i++) bytes[i] = (i * 7) & 255;
    eq(writeFile(P("big.bin"), bytes, { atomic: true }), bytes.length, "a 3 MiB write returns its length");
    const back = readFile(P("big.bin"), { bytes: true });
    ok(back.length === bytes.length && back.every((b, i) => b === bytes[i]), "a 3 MiB binary write round-trips");
    eq(writeFile(P("empty.txt"), "", { atomic: true }), 0, "an empty write");
    eq(stat(P("empty.txt")).size, 0, "creates an empty file");
});
__kitS.section("TypeError: writeFile: atomic and append cannot be combined", () => {
    eq(err(() => writeFile(P("x.txt"), "x", { atomic: true, append: true })), "TypeError: writeFile: atomic and append cannot be combined", "atomic + append is refused");
    ok(!names().includes("x.txt"), "and nothing was created");
    eq(err(() => writeFile(dir, "x", { atomic: true })), "Error: EISDIR", "a directory target is refused");
    {
        const parent = D.slice(0, D.lastIndexOf("/")), base = D.slice(D.lastIndexOf("/") + 1);
        const left = readDir(new Path(parent)).map((e) => e.name).filter((f) => f.startsWith(base + ".tmp-"));
        eq(left, [], "the failed rename removed its temporary (it sat in the parent directory)");
    }
    eq(err(() => writeFile(P("nodir/x.txt"), "x", { atomic: true })), "Error: ENOENT", "a missing parent directory is ENOENT");
    writeFile(P("keep.txt"), "keep me");
    chmod(dir, 0o555);
    const bypass = err(() => writeFile(P("probe.txt"), "p")) === "no throw";
    const e = bypass ? "" : err(() => writeFile(P("keep.txt"), "clobber", { atomic: true }));
    chmod(dir, 0o755);
    if (bypass) {
        removeAll(P("probe.txt"));
        print("SKIP: this process can write into a 0555 directory (root), so the EACCES checks cannot run here");
    } else {
        eq(e, "Error: EACCES", "an unwritable directory fails up front");
        eq(String(readFile(P("keep.txt"))), "keep me", "and the target is untouched");
    }
    eq(err(() => writeFile(P("y.txt"), "y", { atomic: 1, bogus: 1 })), "TypeError: unknown option \"bogus\" (valid: append, atomic)", "unknown options are still refused");
});
__kitS.end();
ok(names().every((f) => !f.includes(".tmp-")), "no temporary file left behind: " + names().join(","));

removeAll(dir);
print("test_file_write_file_atomic: " + n + " checks, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
