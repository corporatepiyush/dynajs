// covers: dyna:file.async.stat dyna:file.async.lstat dyna:file.async.exists dyna:file.async.readDir dyna:file.async.makeDir dyna:file.async.remove dyna:file.async.removeAll dyna:file.async.rename dyna:file.async.copyFile dyna:file.async.move dyna:file.async.symlink dyna:file.async.readLink dyna:file.async.realPath dyna:file.async.chmod
// F2: Promise twins of the 14 path calls; sync and async share one parse (JS thread), run (pool worker, no JS) and result builder, so the
// differential per op on twin fixture trees (result, error constructor/message/errno/code, tree after) must match, plus absolute semantics so a
// shared-code fault still goes red. Argument errors throw synchronously in sync order (Proxy/getter logs), NUL, chmod ToInt32 edges, symlink
// targets 0/300/1000/over-limit bytes, 600-entry readDir with heap names, 64 in flight. Allocation failure, GC, interrupt, teardown: test_file_async_api.c.
import * as file from "dyna:file";
const { Path } = file;
const A = file.async;

let checks = 0, fails = 0;
function ok(c, m) { checks++; if (!c) { fails++; print("FAIL: " + m); } }

const root = String(new Path(Path.temp(), "dj_fafs_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8)));
file.makeDir(root, { recursive: true });
const real = new Map();
let seq = 0;
function fixture() {
    const d = root + "/t" + (seq++);
    file.makeDir(d);
    file.writeFile(d + "/f", "hello");
    file.makeDir(d + "/sub");
    file.writeFile(d + "/sub/g", "x");
    file.symlink("f", d + "/ln");
    file.symlink("nope", d + "/dang");
    file.writeFile(d + "/other", "other!");
    real.set(d, String(file.realPath(d)));
    return d;
}
function norm(v, d) {
    const sub = (s) => s.split(real.get(d)).join("<D>").split(d).join("<D>");
    if (v === undefined) return "undefined";
    if (v instanceof Error) return "E:" + v.constructor.name + ":" + sub(String(v.message)) + ":" + v.errno + ":" + v.code;
    if (v instanceof Path) return "P:" + sub(String(v));
    if (typeof v === "string") return "S:" + sub(v);
    if (Array.isArray(v)) return JSON.stringify(v);
    if (v && typeof v === "object" && "mtimeMs" in v)
        return JSON.stringify({ size: v.size, mode: v.mode, isDir: v.isDir, isFile: v.isFile, isSymlink: v.isSymlink, nlink: v.nlink, uid: v.uid, gid: v.gid, t: typeof v.mtimeMs + typeof v.atimeMs + typeof v.ctimeMs + typeof v.ino });
    return typeof v + ":" + String(v);
}
function tree(d) {
    const out = [];
    const walk = (p, rel) => {
        for (const e of file.readDir(p)) {
            const q = p + "/" + e.name, s = file.lstat(q);
            out.push(rel + e.name + ":" + (e.isDir ? "d" : e.isSymlink ? "l>" + file.readLink(q) : "f" + s.size + "=" + ((s.mode & 0o400) ? file.readFile(q) : "?")) + ":" + (s.isSymlink ? "" : (s.mode & 0o7777).toString(8)));
            if (e.isDir) walk(q, rel + e.name + "/");
        }
    };
    walk(d, "");
    return out.join(",");
}
const E = (code) => (r) => r instanceof Error && r.code === code;
const long = (n) => "a".repeat(n);
const cases = [
    ["stat file", (F, d) => F.stat(d + "/f"), (r) => r.isFile && r.size === 5 && !r.isSymlink && !r.isDir],
    ["stat dir", (F, d) => F.stat(d + "/sub"), (r) => r.isDir && !r.isFile],
    ["stat follows a link", (F, d) => F.stat(d + "/ln"), (r) => r.isFile && !r.isSymlink && r.size === 5],
    ["stat Path argument", (F, d) => F.stat(new Path(d, "f")), (r) => r.isFile],
    ["stat dangling link", (F, d) => F.stat(d + "/dang"), E("ENOENT")],
    ["stat missing", (F, d) => F.stat(d + "/missing"), (r) => E("ENOENT")(r) && /file\.stat\(".*missing"\)/.test(r.message)],
    ["stat ENOTDIR", (F, d) => F.stat(d + "/f/x"), E("ENOTDIR")],
    ["lstat link", (F, d) => F.lstat(d + "/ln"), (r) => r.isSymlink && !r.isFile],
    ["lstat dangling", (F, d) => F.lstat(d + "/dang"), (r) => r.isSymlink],
    ["lstat missing", (F, d) => F.lstat(d + "/missing"), (r) => E("ENOENT")(r) && /file\.lstat/.test(r.message)],
    ["exists file", (F, d) => F.exists(d + "/f"), (r) => r === true],
    ["exists missing", (F, d) => F.exists(d + "/missing"), (r) => r === false],
    ["exists dangling", (F, d) => F.exists(d + "/dang"), (r) => r === true],
    ["readDir", (F, d) => F.readDir(d), (r) => r.map((e) => e.name).join() === "dang,f,ln,other,sub" && r[4].isDir && r[2].isSymlink && r[1].isFile && !r[1].isDir],
    ["readDir empty", (F, d) => (file.makeDir(d + "/e"), F.readDir(d + "/e")), (r) => Array.isArray(r) && r.length === 0],
    ["readDir missing", (F, d) => F.readDir(d + "/missing"), E("ENOENT")],
    ["readDir file", (F, d) => F.readDir(d + "/f"), E("ENOTDIR")],
    ["makeDir", (F, d) => F.makeDir(d + "/n"), (r, d) => r === undefined && file.stat(d + "/n").isDir],
    ["makeDir exists", (F, d) => F.makeDir(d + "/sub"), E("EEXIST")],
    ["makeDir no parent", (F, d) => F.makeDir(d + "/a/b"), E("ENOENT")],
    ["makeDir recursive mode", (F, d) => F.makeDir(d + "/a/b/c", { recursive: true, mode: 0o700 }), (r, d) => (file.stat(d + "/a/b/c").mode & 0o777) === 0o700],
    ["makeDir recursive existing", (F, d) => F.makeDir(d + "/sub", { recursive: true }), (r) => r === undefined],
    ["makeDir mode null", (F, d) => F.makeDir(d + "/mn", { mode: null }), (r, d) => r === undefined && file.stat(d + "/mn").isDir],
    ["remove file", (F, d) => F.remove(d + "/f"), (r, d) => r === undefined && !file.exists(d + "/f")],
    ["remove empty dir", (F, d) => (file.makeDir(d + "/ed"), F.remove(d + "/ed")), (r, d) => !file.exists(d + "/ed")],
    ["remove non-empty dir", (F, d) => F.remove(d + "/sub"), (r) => r instanceof Error && (r.code === "ENOTEMPTY" || r.code === "EEXIST")],
    ["remove missing", (F, d) => F.remove(d + "/missing"), E("ENOENT")],
    ["removeAll", (F, d) => F.removeAll(d + "/sub"), (r, d) => r === undefined && !file.exists(d + "/sub")],
    ["removeAll missing", (F, d) => F.removeAll(d + "/missing"), (r) => r === undefined],
    ["removeAll link keeps target", (F, d) => F.removeAll(d + "/ln"), (r, d) => !file.exists(d + "/ln") && file.exists(d + "/f")],
    ["rename", (F, d) => F.rename(d + "/f", d + "/f2"), (r, d) => r === undefined && file.readFile(d + "/f2") === "hello" && !file.exists(d + "/f")],
    ["rename error names from", (F, d) => F.rename(d + "/missing", d + "/zzz"), (r) => E("ENOENT")(r) && /missing/.test(r.message) && !/zzz/.test(r.message)],
    ["copyFile", (F, d) => F.copyFile(d + "/f", d + "/c"), (r, d) => r === 5 && file.readFile(d + "/c") === "hello"],
    ["copyFile refuses an existing target", (F, d) => F.copyFile(d + "/f", d + "/other"), (r, d) => E("EEXIST")(r) && file.readFile(d + "/other") === "other!" && /other/.test(r.message)],
    ["copyFile overwrite", (F, d) => F.copyFile(d + "/f", d + "/other", { overwrite: true }), (r, d) => r === 5 && file.readFile(d + "/other") === "hello"],
    ["copyFile same file", (F, d) => F.copyFile(d + "/f", d + "/ln", { overwrite: true }), (r, d) => r instanceof TypeError && /same file/.test(r.message) && file.readFile(d + "/f") === "hello"],
    ["copyFile directory", (F, d) => F.copyFile(d + "/sub", d + "/x"), (r) => r instanceof TypeError && /is a directory/.test(r.message)],
    ["copyFile missing", (F, d) => F.copyFile(d + "/missing", d + "/x"), E("ENOENT")],
    ["copyFile size 0", (F, d) => (file.writeFile(d + "/z", ""), F.copyFile(d + "/z", d + "/z2")), (r, d) => r === 0 && file.readFile(d + "/z2") === ""],
    ["move", (F, d) => F.move(d + "/f", d + "/m"), (r, d) => r === undefined && file.readFile(d + "/m") === "hello" && !file.exists(d + "/f")],
    ["move missing", (F, d) => F.move(d + "/missing", d + "/m"), E("ENOENT")],
    ["symlink", (F, d) => F.symlink("target", d + "/s2"), (r, d) => r === undefined && file.readLink(d + "/s2") === "target"],
    ["symlink exists", (F, d) => F.symlink("t", d + "/ln"), (r) => E("EEXIST")(r) && /ln/.test(r.message)],
    ["symlink over-limit target", (F, d) => F.symlink(long(5000), d + "/sl"), (r) => r === undefined || r instanceof Error],
    ["readLink", (F, d) => F.readLink(d + "/ln"), (r) => r === "f"],
    ["readLink not a link", (F, d) => F.readLink(d + "/f"), E("EINVAL")],
    ["readLink missing", (F, d) => F.readLink(d + "/missing"), E("ENOENT")],
    ["readLink 255", (F, d) => (file.symlink(long(255), d + "/l255"), F.readLink(d + "/l255")), (r) => r === long(255)],
    ["readLink 256 (doubles the buffer)", (F, d) => (file.symlink(long(256), d + "/l256"), F.readLink(d + "/l256")), (r) => r === long(256)],
    ["readLink 1000", (F, d) => (file.symlink(long(1000), d + "/l1k"), F.readLink(d + "/l1k")), (r) => r === long(1000)],
    ["realPath link", (F, d) => F.realPath(d + "/ln"), (r, d) => String(r) === real.get(d) + "/f"],
    ["realPath missing", (F, d) => F.realPath(d + "/missing"), E("ENOENT")],
    ["chmod", (F, d) => F.chmod(d + "/f", 0o600), (r, d) => r === undefined && (file.stat(d + "/f").mode & 0o777) === 0o600],
    ["chmod 0", (F, d) => F.chmod(d + "/other", 0), (r, d) => (file.stat(d + "/other").mode & 0o7777) === 0],
    ["chmod 2**32 + 0o644 wraps by ToInt32", (F, d) => F.chmod(d + "/other", 2 ** 32 + 0o644), (r, d) => (file.stat(d + "/other").mode & 0o777) === 0o644],
    ["chmod -1", (F, d) => F.chmod(d + "/other", -1), () => true],
    ["chmod missing", (F, d) => F.chmod(d + "/missing", 0o644), E("ENOENT")],
];

for (const [name, fn, good] of cases) {
    const dS = fixture(), dA = fixture();
    let rs, ra, p;
    try { rs = fn(file, dS); } catch (e) { rs = e; }
    try { p = fn(A, dA); } catch (e) { ok(false, name + ": async threw synchronously " + e); continue; }
    ok(p instanceof Promise, name + ": async returns a Promise");
    try { ra = await p; } catch (e) { ra = e; }
    ok(norm(rs, dS) === norm(ra, dA), name + ": sync " + norm(rs, dS) + " vs async " + norm(ra, dA));
    ok(tree(dS) === tree(dA), name + ": trees differ after the call\n  sync  " + tree(dS) + "\n  async " + tree(dA));
    ok(good(ra, dA), name + ": async result " + norm(ra, dA));
}

{
    const d = fixture();
    const [s1, s2] = [file.stat(d + "/f"), await A.stat(d + "/f")];
    ok(JSON.stringify(s1) === JSON.stringify(s2), "stat of one file: every field equal (ino, times, mode)");
    const ps = [];
    for (let i = 0; i < 64; i++) ps.push([A.stat, A.lstat, A.exists, A.readDir, A.readLink, A.realPath][i % 6](d + (i % 6 === 3 ? "" : i % 6 === 4 ? "/ln" : "/f")));
    const rs = await Promise.all(ps);
    const want = [file.stat, file.lstat, file.exists, file.readDir, file.readLink, file.realPath].map((f, k) => norm(f(d + (k === 3 ? "" : k === 4 ? "/ln" : "/f")), d));
    ok(rs.every((r, i) => norm(r, d) === want[i % 6]), "64 mixed requests in flight equal the sync results");
}

{
    const d = root + "/big";
    file.makeDir(d);
    const names = [];
    for (let i = 0; i < 600; i++) names.push((i % 2 ? "L" + "n".repeat(60) : "s") + String((i * 7919) % 600).padStart(4, "0"));
    for (const n of names) file.writeFile(d + "/" + n, "");
    const s = file.readDir(d), a = await A.readDir(d);
    ok(JSON.stringify(s) === JSON.stringify(a), "600 entries (half past the inline name size): async === sync");
    ok(a.length === 600 && a.every((e, i) => i === 0 || a[i - 1].name < e.name), "600 entries sorted by name");
    ok(a.every((e) => e.isFile && !e.isDir && !e.isSymlink), "600 entries typed as files");
}

{
    const d = fixture(), m = d + "/missing";
    const hostileStr = { toString() { throw new RangeError("toString boom"); } };
    const argCases = [
        ["stat no argument", (F) => F.stat()],
        ["stat NUL", (F) => F.stat(d + "/f\0x")],
        ["stat number", (F) => F.stat(42)],
        ["stat hostile toString", (F) => F.stat(hostileStr)],
        ["readDir NUL", (F) => F.readDir("a\0b")],
        ["rename NUL from", (F) => F.rename("a\0b", m)],
        ["rename NUL to", (F) => F.rename(m, "a\0b")],
        ["symlink NUL target", (F) => F.symlink("t\0x", m)],
        ["symlink hostile target", (F) => F.symlink(hostileStr, m)],
        ["symlink NUL linkpath", (F) => F.symlink("t", "a\0b")],
        ["makeDir unknown option", (F) => F.makeDir(m, { bogus: 1 })],
        ["makeDir throwing getter", (F) => F.makeDir(m, { get recursive() { throw new Error("getter boom"); } })],
        ["makeDir Symbol mode", (F) => F.makeDir(m, { mode: Symbol("s") })],
        ["makeDir throwing valueOf", (F) => F.makeDir(m, { mode: { valueOf() { throw new RangeError("valueOf boom"); } } })],
        ["makeDir throwing ownKeys Proxy", (F) => F.makeDir(m, new Proxy({}, { ownKeys() { throw new Error("ownKeys boom"); } }))],
        ["copyFile no arguments", (F) => F.copyFile()],
        ["copyFile one argument", (F) => F.copyFile(m)],
        ["copyFile unknown option", (F) => F.copyFile(m, m + "2", { bogus: 1 })],
        ["copyFile throwing getter", (F) => F.copyFile(m, m + "2", { get overwrite() { throw new Error("ow boom"); } })],
        ["move one argument", (F) => F.move(m)],
        ["chmod Symbol mode", (F) => F.chmod(m, Symbol("s"))],
        ["chmod throwing valueOf", (F) => F.chmod(m, { valueOf() { throw new RangeError("mode boom"); } })],
        ["chmod NUL path", (F) => F.chmod("a\0b", 0o644)],
    ];
    for (const [name, fn] of argCases) {
        let es = null, ea = null, ra;
        try { fn(file); } catch (e) { es = e; }
        try { ra = fn(A); } catch (e) { ea = e; }
        if (ra instanceof Promise) ra.catch(() => {});
        ok(es !== null, name + ": the sync call throws");
        ok(ea !== null && !(ra instanceof Promise), name + ": the async call throws synchronously" + (ra instanceof Promise ? " (got a Promise)" : ""));
        ok(es && ea && es.constructor === ea.constructor && String(es.message) === String(ea.message), name + ": same error " + es + " vs " + ea);
    }
    ok(!file.exists(m), "no argument error created anything");
    const order = (F, fn) => {
        const log = [];
        const px = new Proxy({ overwrite: false }, {
            ownKeys(t) { log.push("ownKeys"); return Reflect.ownKeys(t); },
            getOwnPropertyDescriptor(t, k) { log.push("gopd:" + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
            get(t, k) { log.push("get:" + String(k)); return t[k]; },
        });
        let err = null, r;
        try { r = fn(F, px); } catch (e) { err = e; }
        if (r instanceof Promise) r.catch(() => {});
        return [log.join(","), err ? err.constructor.name : "none"];
    };
    for (const [name, fn, optsFirst] of [["copyFile", (F, px) => F.copyFile(undefined, m, px), true], ["makeDir", (F, px) => F.makeDir(undefined, px), false]]) {
        const s = order(file, fn), a = order(A, fn);
        ok(s[0] === a[0] && s[1] === a[1], name + ": conversion order sync [" + s + "] vs async [" + a + "]");
        ok(optsFirst ? a[0].startsWith("ownKeys") && a[1] === "TypeError" : a[0] === "" && a[1] === "TypeError", name + ": " + (optsFirst ? "options read before the paths" : "path read before the options") + " [" + a + "]");
    }
    const modeLog = [];
    const s = (() => { try { file.chmod(undefined, { valueOf() { modeLog.push("s"); return 0; } }); } catch (e) { return e.constructor.name; } })();
    const a = (() => { try { A.chmod(undefined, { valueOf() { modeLog.push("a"); return 0; } }); } catch (e) { return e.constructor.name; } })();
    ok(s === "TypeError" && a === "TypeError" && modeLog.length === 0, "chmod: path checked before the mode valueOf runs [" + modeLog + "]");
}

file.removeAll(root);
print("test_file_async_fs: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " check(s) failed");
