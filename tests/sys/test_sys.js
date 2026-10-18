import {
    stat, lstat, exists,
    readDir, makeDir, remove, removeAll, rename,
    symlink, readLink, realPath, chmod,
    glob,
    tempDir, makeTempDir, makeTempFile,
    Path,
} from "dyna:file";
import {
    env, getEnv, setEnv, args, cwd, chDir, platform, pid, hostName, homeDir,
} from "dyna:sys";
import { writeFile } from "dyna:file";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught !== null, msg + " (expected throw)");
    assert(caught instanceof Error, msg + " (throws an Error, got " + caught + ")");
    return caught;
}
function throwsCode(fn, code, msg) {
    const e = throws(fn, msg);
    assert(e.code === code, msg + " (code " + e.code + " expected " + code + ")");
}
function eqArray(actual, expected, msg) {
    assert(Array.isArray(actual), msg + " (is array, got " + typeof actual + ")");
    assert(actual.length === expected.length,
        msg + " (length " + actual.length + " vs " + expected.length +
        "): [" + actual.join(",") + "]");
    for (let i = 0; i < expected.length; i++)
        assert(String(actual[i]) === String(expected[i]),
            msg + " [" + i + "]: '" + actual[i] + "' vs '" + expected[i] + "'");
}

const origCwd = cwd();
const root = makeTempDir("dyna-sys-test-");
let passed = false;

try {
    assert(Path.isPath(root) && String(root).length > 0, "makeTempDir returns a Path");
    assert(String(root).indexOf("dyna-sys-test-") >= 0, "temp dir carries the prefix");
    assert(exists(root), "fresh temp dir exists");
    {
        const s = stat(root);
        assert(s.isDir === true, "root isDir");
        assert(s.isFile === false, "root not isFile");
        assert(s.isSymlink === false, "root not symlink (via stat)");
        assert(typeof s.mode === "number", "mode is a number");
        assert(typeof s.mtimeMs === "number" && s.mtimeMs > 1e12, "mtimeMs sane");
        assert(typeof s.atimeMs === "number" && s.atimeMs > 1e12, "atimeMs sane");
        assert(typeof s.ctimeMs === "number" && s.ctimeMs > 1e12, "ctimeMs sane");
        assert(typeof s.uid === "number" && s.uid >= 0, "uid sane");
        assert(typeof s.gid === "number" && s.gid >= 0, "gid sane");
        assert(typeof s.ino === "number" && s.ino > 0, "ino sane");
        assert(typeof s.nlink === "number" && s.nlink >= 1, "nlink >= 1");
        assert(typeof s.size === "number", "size is a number");
    }

    {
        const plat = platform();
        assert(plat === "darwin" || plat === "linux", "platform is darwin/linux: " + plat);
        assert(Number.isInteger(pid()) && pid() > 0, "pid is a positive integer");
        assert(typeof hostName() === "string" && hostName().length > 0, "hostName non-empty");
        const a = args();
        assert(Array.isArray(a) && a.length >= 1, "args is a non-empty array");
        assert(typeof a[0] === "string" && a[0].length > 0, "args[0] is a string");
        const c = cwd();
        assert(typeof c === "string" && c[0] === "/", "cwd is an absolute path");
        assert(Path.isPath(tempDir()) && String(tempDir()).length > 0, "tempDir returns a Path");
        assert(String(tempDir()).slice(-1) !== "/" || String(tempDir()) === "/", "tempDir has no trailing slash");
        assert(String(Path.temp()) === String(tempDir()), "Path.temp() and tempDir() agree, trailing separator included");
        assert(typeof homeDir() === "string" && homeDir().length > 0, "homeDir non-empty");
        const e = env();
        assert(typeof e === "object" && e !== null, "env() is an object");
        assert(!Array.isArray(e), "env() is not an array");
    }

    {
        const key = "DYNAJS_SYS_TEST_" + pid();
        assert(getEnv(key) === undefined, "unset env var is undefined");
        setEnv(key, "hello123");
        assert(getEnv(key) === "hello123", "getEnv sees setEnv value");
        assert(env()[key] === "hello123", "env() reflects setEnv value");
        setEnv(key, "changed-value");
        assert(getEnv(key) === "changed-value", "setEnv overwrites");
        setEnv(key, "");
        assert(getEnv(key) === "", "setEnv empty string round-trips");
        assert(getEnv("DYNAJS_SYS_DEFINITELY_UNSET_" + pid()) === undefined,
            "random unset var undefined");
    }

    {
        const key = "DYNAJS_SYS_NUL_" + pid();
        throws(() => setEnv("A\u0000B", "v"),
            "an embedded NUL in the name is refused");
        throws(() => setEnv(key, "x\u0000y"),
            "an embedded NUL in the value is refused");
        assert(getEnv(key) === undefined, "the refused value was never set");
        throws(() => setEnv("A=B", "v"), "a '=' in the name is refused");
        throws(() => setEnv("", "v"), "an empty name is refused");
        assert(getEnv("A=B") === undefined, "nothing was set under 'A=B'");
    }

    {
        const sub = root.join("sub");
        makeDir(sub);
        assert(exists(sub) && stat(sub).isDir, "makeDir creates a directory");

        const deep = root.join("a/b/c");
        makeDir(deep, { recursive: true });
        assert(exists(deep) && stat(deep).isDir, "makeDir recursive creates nested dirs");
        assert(exists(root.join("a")) && exists(root.join("a/b")), "recursive makes all parents");

        makeDir(deep, { recursive: true });
        assert(exists(deep), "makeDir recursive on existing is a no-op");

        throwsCode(() => makeDir(sub), "EEXIST", "makeDir non-recursive on existing throws");

        throwsCode(() => makeDir(root.join("nope/child")), "ENOENT",
            "makeDir non-recursive missing parent throws");
    }

    {
        const f = root.join("hello.txt");
        writeFile(f, "hello world");
        const s = stat(f);
        assert(s.isFile === true, "file isFile");
        assert(s.isDir === false, "file not isDir");
        assert(s.isSymlink === false, "file not symlink");
        assert(s.size === 11, "file size is 11 (got " + s.size + ")");
        assert(s.nlink >= 1, "file nlink >= 1");
        const ls = lstat(f);
        assert(ls.isFile === true && ls.isSymlink === false, "lstat regular file");

        assert(exists(f) === true, "exists true for a real file");
        assert(exists(root.join("no-such-file")) === false, "exists false for missing");
    }

    {
        const f = root.join("hello.txt");
        const modes = [0o600, 0o644, 0o755, 0o700, 0o640];
        for (const m of modes) {
            chmod(f, m);
            assert((stat(f).mode & 0o777) === m,
                "chmod sets perm bits to 0o" + m.toString(8) +
                " (got 0o" + (stat(f).mode & 0o777).toString(8) + ")");
        }
        assert((stat(f).mode & 0o170000) !== 0, "full st_mode carries type bits");
    }

    {
        const link = root.join("link.txt");
        symlink("hello.txt", link);
        assert(lstat(link).isSymlink === true, "lstat sees the symlink");
        assert(stat(link).isFile === true, "stat follows the symlink to a file");
        assert(readLink(link) === "hello.txt", "readLink returns the target");
        assert(realPath(link).equals(realPath(root.join("hello.txt"))),
            "realPath resolves the symlink to the same canonical path");

        const dangling = root.join("dangling");
        symlink("target-does-not-exist", dangling);
        assert(exists(dangling) === true, "exists true for a dangling symlink (lstat)");
        assert(lstat(dangling).isSymlink === true, "lstat sees the dangling symlink");
        throws(() => stat(dangling), "stat on a dangling symlink throws");
        assert(readLink(dangling) === "target-does-not-exist", "readLink of dangling link");
    }

    {
        const rd = root.join("rd");
        makeDir(rd);
        writeFile(rd.join("a.txt"), "A");
        writeFile(rd.join("b.txt"), "BB");
        makeDir(rd.join("d"));
        symlink("a.txt", rd.join("s"));
        const ents = readDir(rd);
        assert(ents.length === 4, "readDir returns 4 entries (got " + ents.length + ")");
        eqArray(ents.map(e => e.name), ["a.txt", "b.txt", "d", "s"], "readDir sorted by name");
        const byName = {};
        for (const e of ents) byName[e.name] = e;
        assert(byName["a.txt"].isFile && !byName["a.txt"].isDir && !byName["a.txt"].isSymlink,
            "a.txt is a file");
        assert(byName["b.txt"].isFile, "b.txt is a file");
        assert(byName["d"].isDir && !byName["d"].isFile, "d is a directory");
        assert(byName["s"].isSymlink && !byName["s"].isDir, "s is a symlink");
        assert(ents.every(e => e.name !== "." && e.name !== ".."),
            "readDir skips . and ..");

        makeDir(root.join("empty-rd"));
        eqArray(readDir(root.join("empty-rd")), [], "readDir of empty dir is []");

        {
            const hostile = root.join("rd-hostile");
            makeDir(hostile);
            writeFile(hostile.join("__proto__"), "x");
            writeFile(hostile.join("constructor"), "y");
            const es = readDir(hostile);
            assert(es.length === 2, "readDir lists hostile names (got " + es.length + ")");
            const names = es.map(e => e.name).sort();
            eqArray(names, ["__proto__", "constructor"], "hostile names survive");
            for (const e of es)
                assert(e.isFile === true && e.isDir === false,
                    "a hostile name is still classified: " + e.name);
        }

        {
            let deep = root.join("rd-many");
            makeDir(deep);
            for (let i = 0; i < 6; i++) { deep = deep.join("segment-with-a-long-name-" + i); makeDir(deep); }
            const N = 300;
            for (let i = 0; i < N; i++) writeFile(deep.join("f" + i + ".bin"), "z");
            makeDir(deep.join("sub"));
            const es = readDir(deep);
            assert(es.length === N + 1,
                "readDir over " + (N + 1) + " entries returns them all, got " + es.length);
            let files = 0, dirs = 0;
            for (const e of es) { if (e.isFile) files++; if (e.isDir) dirs++; }
            assert(files === N, "every regular file is isFile (got " + files + ")");
            assert(dirs === 1, "and the one subdirectory is isDir (got " + dirs + ")");
            const uniq = new Set(es.map(e => e.name));
            assert(uniq.size === es.length, "no entry is duplicated by the growth path");
            for (let i = 1; i < es.length; i++)
                assert(es[i - 1].name < es[i].name, "readDir stays sorted at scale");
        }
    }

    {
        const oldp = root.join("old.txt"), newp = root.join("renamed.txt");
        writeFile(oldp, "xyz");
        rename(oldp, newp);
        assert(exists(oldp) === false, "rename removes the source name");
        assert(exists(newp) === true, "rename creates the destination");
        assert(stat(newp).size === 3, "renamed file keeps its content");
        remove(newp);
    }

    {
        const f = root.join("rm.txt");
        writeFile(f, "y");
        remove(f);
        assert(!exists(f), "remove deletes a file");

        const d = root.join("rmdir");
        makeDir(d);
        remove(d);
        assert(!exists(d), "remove deletes an empty directory");

        const full = root.join("full");
        makeDir(full);
        writeFile(full.join("f"), "z");
        throws(() => remove(full), "remove of a non-empty dir throws");
        throwsCode(() => remove(root.join("missing-xyz")), "ENOENT", "remove of a missing path throws");
        removeAll(full);
        assert(!exists(full), "removeAll cleans the non-empty dir");
    }

    {
        writeFile(root.join("external.txt"), "keep-me");
        makeDir(root.join("extdir"));
        writeFile(root.join("extdir/keep.txt"), "keep-me-too");

        makeDir(root.join("tree/a/b/c"), { recursive: true });
        writeFile(root.join("tree/f0"), "0");
        writeFile(root.join("tree/a/f1"), "1");
        writeFile(root.join("tree/a/b/f2"), "2");
        writeFile(root.join("tree/a/b/c/f3"), "3");
        symlink(root.join("external.txt"), root.join("tree/a/linkout"));
        symlink(root.join("extdir"), root.join("tree/a/b/dirlink"));
        assert(exists(root.join("tree/a/b/c/f3")), "deep file created");
        assert(lstat(root.join("tree/a/linkout")).isSymlink, "linkout is a symlink");

        removeAll(root.join("tree"));
        assert(exists(root.join("tree")) === false, "removeAll removes the whole tree");
        assert(exists(root.join("external.txt")) === true,
            "removeAll is symlink-safe: external file survived");
        assert(exists(root.join("extdir")) === true && exists(root.join("extdir/keep.txt")) === true,
            "removeAll is symlink-safe: external dir + its contents survived");

        removeAll(root.join("does-not-exist"));
        assert(true, "removeAll of a missing path is a no-op");

        writeFile(root.join("single.txt"), "s");
        removeAll(root.join("single.txt"));
        assert(!exists(root.join("single.txt")), "removeAll deletes a single file");

        symlink(root.join("external.txt"), root.join("toplink"));
        removeAll(root.join("toplink"));
        assert(exists(root.join("toplink")) === false, "removeAll deletes the symlink");
        assert(exists(root.join("external.txt")) === true, "removeAll did not follow the symlink");

        remove(root.join("external.txt"));
        removeAll(root.join("extdir"));
    }

    {
        const g = root.join("glob");
        makeDir(g);
        for (const name of ["one.txt", "two.txt", "three.md",
                            "abc", "axc", "azc", "ab", "bob", "cat", "dog",
                            ".hidden", "top.js"])
            writeFile(g.join(name), "x");
        makeDir(g.join("sub"));
        writeFile(g.join("sub/nested.js"), "x");
        makeDir(g.join("sub/deep"));
        writeFile(g.join("sub/deep/deeper.js"), "x");

        eqArray(glob("*.txt", { cwd: g }), ["one.txt", "two.txt"], "glob *.txt");
        eqArray(glob("*.js", { cwd: g }), ["top.js"], "glob *.js (single segment, no cross-dir)");
        eqArray(glob("**/*.js", { cwd: g }),
            ["sub/deep/deeper.js", "sub/nested.js", "top.js"], "glob **/*.js");
        eqArray(glob("a?c", { cwd: g }), ["abc", "axc", "azc"], "glob a?c");
        eqArray(glob("?ne.txt", { cwd: g }), ["one.txt"], "glob ?ne.txt");
        eqArray(glob("[abc]*", { cwd: g }),
            ["ab", "abc", "axc", "azc", "bob", "cat"], "glob [abc]*");
        eqArray(glob("[a-c]*", { cwd: g }),
            ["ab", "abc", "axc", "azc", "bob", "cat"], "glob [a-c]* (range)");
        eqArray(glob("[!a-c]*", { cwd: g }),
            ["dog", "one.txt", "sub", "three.md", "top.js", "two.txt"],
            "glob [!a-c]* (negated range)");
        eqArray(glob("*/nested.js", { cwd: g }), ["sub/nested.js"], "glob */nested.js");
        eqArray(glob("*/*.js", { cwd: g }), ["sub/nested.js"], "glob */*.js");
        eqArray(glob("*.xyz", { cwd: g }), [], "glob with no match is []");
        eqArray(glob("nope/**", { cwd: g }), [], "glob under a missing dir is []");

        assert(glob("*", { cwd: g }).map(String).indexOf(".hidden") === -1, "glob * skips dotfiles");
        assert(glob("*", { cwd: g }).map(String).indexOf("one.txt") >= 0, "glob * includes plain files");

        {
            const all = glob("**", { cwd: g }).map(String);
            assert(all.length === 15, "glob ** count is 15 (got " + all.length + ")");
            for (const want of ["sub", "sub/deep", "sub/deep/deeper.js",
                                "sub/nested.js", "one.txt", "top.js", "abc"])
                assert(all.indexOf(want) >= 0, "glob ** includes " + want);
            assert(all.indexOf(".hidden") === -1, "glob ** skips dotfiles");
            assert(all.indexOf(".") === -1, "glob ** does not emit .");
            assert(all.every(p => p[0] !== "."), "glob ** emits no dot-prefixed path");
        }

        eqArray(glob("sub/nested.js", { cwd: g }), ["sub/nested.js"], "glob literal nested path");
        eqArray(glob("sub", { cwd: g }), ["sub"], "glob literal directory name");
        eqArray(glob(".hidden", { cwd: g }), [".hidden"], "glob explicit dotfile name");
        eqArray(glob("no-such-thing", { cwd: g }), [], "glob literal missing name is []");

        {
            const abs = glob(g.join("*.txt"));
            assert(abs.length === 2, "absolute glob returns 2 results");
            assert(abs.every(p => p.isAbsolute), "absolute glob results are absolute");
            assert(abs[0].basename === "one.txt" && abs[1].basename === "two.txt",
                "absolute glob resolves the right names");
            assert(abs[0].equals(g.join("one.txt")) && abs[1].equals(g.join("two.txt")),
                "absolute glob reconstructs the full path");
        }

        {
            const save = cwd();
            chDir(String(g));
            eqArray(glob("*.txt"), ["one.txt", "two.txt"], "glob default cwd is current dir");
            chDir(String(save));
        }

        removeAll(g);
        assert(!exists(g), "glob tree cleaned");
    }

    {
        const gc = root.join("globcycle");
        makeDir(gc.join("inner"), { recursive: true });
        writeFile(gc.join("inner/leaf.txt"), "L");
        symlink(String(gc), gc.join("inner/loop"));

        const all = glob("**", { cwd: gc }).map(String);
        assert(Array.isArray(all), "glob ** over a cycle terminates (returns an array)");
        assert(all.indexOf("inner") >= 0 && all.indexOf("inner/leaf.txt") >= 0,
            "glob ** over a cycle still finds the real entries");
        assert(all.indexOf("inner/loop") >= 0, "glob ** emits the symlink as a leaf");
        assert(all.every(p => p.indexOf("loop/") === -1),
            "glob ** never descends through the symlink loop");
        eqArray(glob("**/*.txt", { cwd: gc }), ["inner/leaf.txt"],
            "glob **/*.txt over a cycle terminates with the real match");

        removeAll(gc);
        assert(!exists(gc), "glob-cycle tree cleaned");
    }

    {
        const td = tempDir();
        assert(exists(td) && stat(td).isDir, "tempDir points at a real directory");

        const tf = makeTempFile("dyna-sys-tf-");
        assert(Path.isPath(tf) && exists(tf) && stat(tf).isFile,
            "makeTempFile creates a Path naming an empty file");
        assert(String(tf).indexOf("dyna-sys-tf-") >= 0, "makeTempFile carries the prefix");
        remove(tf);
        assert(!exists(tf), "makeTempFile cleaned up");

        const tdir = makeTempDir("dyna-sys-td-");
        assert(Path.isPath(tdir) && exists(tdir) && stat(tdir).isDir,
            "makeTempDir creates a Path naming a directory");
        assert(!tdir.equals(root), "each makeTempDir is unique");
        removeAll(tdir);
        assert(!exists(tdir), "makeTempDir cleaned up");
    }

    {
        const before = cwd();
        chDir(String(root));
        assert(realPath(new Path(cwd())).equals(realPath(root)),
            "chDir changes cwd to the target");
        chDir(before);
        assert(cwd() === before, "chDir restores the original cwd");
        throwsCode(() => chDir(String(root.join("no-such-dir"))), "ENOENT", "chDir to a missing dir throws");
        throws(() => chDir(String(root) + "\u0000/nowhere"),
               "chDir refuses a NUL-bearing path");
        assert(cwd() === before, "and the refused chDir moved nowhere");
        throws(() => getEnv("DYNAJS_SYS_TEST_" + pid() + "\u0000suffix"),
               "getEnv refuses a NUL-bearing name (no env name can contain one)");
    }

    {
        const missing = root.join("definitely-missing");
        throwsCode(() => stat(missing), "ENOENT", "stat missing throws ENOENT");
        throwsCode(() => lstat(missing), "ENOENT", "lstat missing throws ENOENT");
        throwsCode(() => readDir(missing), "ENOENT", "readDir missing throws ENOENT");
        throwsCode(() => realPath(missing), "ENOENT", "realPath missing throws ENOENT");
        throwsCode(() => chmod(missing, 0o644), "ENOENT", "chmod missing throws ENOENT");
        throwsCode(() => rename(missing, root.join("x")), "ENOENT", "rename missing source throws");

        writeFile(root.join("plain.txt"), "p");
        const e = throws(() => readLink(root.join("plain.txt")), "readLink of a non-symlink throws");
        assert(typeof e.message === "string" && e.message.indexOf("readLink") >= 0,
            "error message names the operation");
        assert(typeof e.errno === "number", "error carries a numeric errno");
        remove(root.join("plain.txt"));

        writeFile(root.join("notdir"), "x");
        throws(() => readDir(root.join("notdir")), "readDir on a file throws");
        remove(root.join("notdir"));
    }

    chDir(origCwd);
    removeAll(root);
    assert(exists(root) === false, "temp root fully removed -- hermetic");

    passed = true;
} finally {
    if (!passed) {
        try { chDir(origCwd); } catch (e) {  }
        try { removeAll(root); } catch (e) {  }
    }
}

print("test_sys: all tests passed (" + n + " assertions)");
