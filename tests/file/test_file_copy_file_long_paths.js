// covers: dyna:file.copyFile
// C2a: copyFile's own TypeErrors ("same file", "is a directory", "is not a regular file") went through JS_ThrowTypeError,
// whose message buffer is 256 bytes, so with a deep TMPDIR the paths were cut and "are the same file" was lost (the
// self-copy case in test_file_copy_file.js once flaked on ASan for that reason). They now use dyn_fs_throw_type, which
// sizes the message to fit; these cases use paths of about 900 bytes (below PATH_MAX on every platform).
import { suite } from "../lib/kit.js";
import { copyFile, writeFile, readFile, makeDir, makeTempDir, removeAll, symlink, Path } from "dyna:file";

const t = suite("test_file_copy_file_long_paths");

let tag = 0;
function deepDir(root, target) {
    let s = root.toString();
    let i = 0;
    tag++;
    while (s.length + 110 < target) {
        s += "/" + tag + "_" + String(i++).padStart(3, "0") + "_" + "d".repeat(96);
        makeDir(new Path(s));
    }
    return s;
}

let root;
t.before(() => { root = makeTempDir("cf-long-"); });
t.after(() => { if (root) removeAll(root); });

t.test("same-file error keeps both long paths and the reason", (a) => {
    const dir = deepDir(root, 900);
    const file = dir + "/self_" + "f".repeat(40) + ".txt";
    a.must(file.length > 800, "path is long: " + file.length);
    writeFile(new Path(file), "PRECIOUS");
    const e = a.throws(() => copyFile(new Path(file), new Path(file), { overwrite: true }), TypeError);
    const want = "copyFile: '" + file + "' and '" + file + "' are the same file";
    a.eq(e && e.message, want, "full message");
    a.ok(e && e.message.length > 256, "longer than the old 256-byte buffer: " + (e && e.message.length));
    a.eq(readFile(new Path(file)), "PRECIOUS", "data intact");
});

t.test("same file reached through a long symlink keeps both paths", (a) => {
    const dir = deepDir(root, 880);
    const file = dir + "/real.txt";
    writeFile(new Path(file), "X");
    const other = dir + "/alias_" + "s".repeat(30) + ".txt";
    symlink(file, new Path(other));
    const e = a.throws(() => copyFile(new Path(file), new Path(other), { overwrite: true }), TypeError);
    a.eq(e && e.message, "copyFile: '" + file + "' and '" + other + "' are the same file");
    a.eq(readFile(new Path(file)), "X", "data intact");
});

t.test("directory source error keeps the long path", (a) => {
    const dir = deepDir(root, 950);
    const e = a.throws(() => copyFile(new Path(dir), new Path(root.toString() + "/out")), TypeError);
    a.eq(e && e.message, "copyFile: '" + dir + "' is a directory");
});

t.test("non-regular source keeps its path", (a) => {
    const e = a.throws(() => copyFile(new Path("/dev/null"), new Path(root.toString() + "/out2")), TypeError);
    a.eq(e && e.message, "copyFile: '/dev/null' is not a regular file");
});

t.test("short paths keep the same message shape", (a) => {
    const f = root.toString() + "/s.txt";
    writeFile(new Path(f), "y");
    const e = a.throws(() => copyFile(new Path(f), new Path(f), { overwrite: true }), TypeError);
    a.eq(e && e.message, "copyFile: '" + f + "' and '" + f + "' are the same file");
    a.ok(e && typeof e.stack === "string", "has a stack");
});

await t.run();
