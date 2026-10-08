// flags: --std
// timeout: 120
import { Path, makeTempDir, makeDir, writeFile, removeAll, exists } from "dyna:file";
import { Spawn, args, Which } from "dyna:sys";
import * as os from "os";

const SELF = scriptArgs[0];
const isChild = scriptArgs.indexOf("--child") >= 0;
const isLongPathChild = scriptArgs.indexOf("--longpath") >= 0;

function build(root, depth) {
    let p = String(root);
    makeDir(new Path(p), { recursive: true });
    for (let i = 0; i < depth; i++) {
        p += "/d";
        makeDir(new Path(p));
        if (i % 7 === 0)
            writeFile(new Path(p + "/f"), "x");
    }
    return p;
}

async function runChild(flags, rlimit, expect) {
    const p = new Spawn("sh",
        ["-c", "ulimit -n " + rlimit + '; exec "$0" --std "$1" ' + flags, args()[0], SELF],
        { timeoutMs: 60000 });
    const buf = new Uint8Array(65536);
    const readAll = async (src) => {
        let s = "";
        for (;;) {
            const k = await src.read(buf);
            if (k === 0)
                return s;
            s += new TextDecoder().decode(buf.subarray(0, k));
        }
    };
    const o = await readAll(p.stdout);
    const e = await readAll(p.stderr);
    const r = await p.wait();
    if (r.code !== 0 || e.indexOf("EMFILE") >= 0 || e.indexOf("Too many open files") >= 0
        || e.indexOf("ENAMETOOLONG") >= 0 || e.indexOf("File name too long") >= 0)
        throw new Error(flags + " under ulimit -n " + rlimit + " failed (code " + r.code + "): " + e);
    if (o.indexOf(expect) < 0)
        throw new Error("child did not report success for " + flags + ": " + o);
}

if (isChild) {
    const root = makeTempDir("rmrf-deep-");
    build(root, 200);
    removeAll(root);
    if (exists(root))
        throw new Error("removeAll left the root behind");
    print("child ok");
} else if (isLongPathChild) {
    // R4-1: nested absolute paths here exceed PATH_MAX (depth 110 times a
    // 16-char component); each directory is created and entered relative to
    // the cwd, which is the only way to build such a tree portably.
    const root = makeTempDir("rmrf-long-");
    if (os.chdir(root) !== 0)
        throw new Error("chdir to the tree root failed");
    for (let i = 0; i < 110; i++) {
        makeDir(new Path("componentname16x"));
        if (os.chdir("componentname16x") !== 0)
            throw new Error("chdir to level " + i + " failed");
    }
    if (os.chdir(String(new Path(root).dirname)) !== 0)
        throw new Error("chdir to the tree parent failed");
    removeAll(root);
    if (exists(root))
        throw new Error("long-path removeAll left the root behind");
    print("longpath ok");
} else {
    if (!Which("sh"))
        throw new Error("no sh");
    await runChild("--child", 64, "child ok");
    await runChild("--longpath", 64, "longpath ok");
    await runChild("--longpath", 128, "longpath ok");
    print("test_fs_rmrf_deep: removed a 200-level tree and a >PATH_MAX tree at ulimit -n 64/128");
}
