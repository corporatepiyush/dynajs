// flags: --std
// covers: dyna:sftp.SFTPServer -- chroot-safe path resolution (no .. or symlink escape), readOnly, the allow hook (sync, async, throwing, non-boolean), serve() misuse
// timeout: 120
// The served root has a sibling directory "outside" with a secret. Every escape route must fail and must leave "outside" untouched:
// "..", "/..", ".." after a real directory, absolute symlinks (resolved inside the root, so "/sub/inner.txt" works), relative
// symlinks that climb out, a symlinked directory that points out, chained links, a symlink loop (ELOOP is a FAILURE, not a
// hang), a symlink the client creates with an escaping target, writes, renames and mkdirs through or toward such paths, and a
// final component swapped for a host symlink by the allow hook between resolution and open (O_NOFOLLOW makes the open fail).
// readOnly refuses every write with PERMISSION_DENIED and still serves reads. The allow hook gets { op, path, target, user,
// write } with path already resolved through symlinks; a Promise is awaited (the session's later requests wait); a throw or a
// non-boolean means deny; denials are counted in the serve() result. An approval from a pending Promise applies only if the
// operation, resolved path and target are unchanged when the request is re-run (a symlink swapped meanwhile is denied). Five sessions
// of denied, failed, paused and never-closed operations leave the process fd count unchanged. serve() outside onSession's synchronous part, with an
// unknown option key, or with a root that is not a directory throws a TypeError.
import * as os from "os";
import { SFTPClient, SFTPServer } from "dyna:sftp";
import { SSHServer, SSHClient } from "dyna:ssh";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir, keygen, write, pubBlob, sleep } from "../ssh/ssh_util.js";
import { bytes, same, readBin, exists, makeTree, sftpHost } from "./sftp_util.js";

const t = suite("sftp_server_chroot");
const hosts = [];
t.after(() => { for (const h of hosts) h.close(); });
const td = new TextDecoder();

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const tree = makeTree("sftpchroot");
    const R = tree.root;
    os.symlink("/outside/secret", R + "/abs");
    os.symlink("../outside/secret", R + "/rel");
    os.symlink("../../../../../../outside", R + "/dl");
    os.symlink("dl", R + "/chain");
    os.symlink("/sub/inner.txt", R + "/absin");
    os.symlink("loop2", R + "/loop1");
    os.symlink("loop1", R + "/loop2");
    os.symlink("../outside/new", R + "/wl");
    const host = (opts = {}) => { const h = sftpHost(tmpDir("sftpc"), typeof opts === "function" ? opts : { root: R, ...opts }); hosts.push(h); return h; };
    const main = host();
    const untouched = (a) => {
        a.deep(os.readdir(tree.outside)[0].filter((x) => x !== "." && x !== "..").sort(), ["secret"], "outside/ is untouched");
        a.eq(td.decode(readBin(tree.outside + "/secret")), "TOP-SECRET\n");
    };

    t.test(".. never leaves the root", async (a) => {
        const { sftp, close } = await main.connect();
        a.eq(await sftp.realpath("../../.."), "/");
        a.eq(await sftp.realpath("/../outside"), "/outside");
        for (const p of ["../outside/secret", "/../outside/secret", "sub/../../outside/secret", "sub/../../../outside/secret", "/../../../../" + tree.outside.slice(1) + "/secret"])
            a.eq((await sftp.readFile(p).catch((e) => e)).code, "NO_SUCH_FILE", p);
        a.eq((await sftp.stat("../outside").catch((e) => e)).code, "NO_SUCH_FILE");
        untouched(a);
        await close();
    });

    t.test("symlinks resolve inside the root", async (a) => {
        const { sftp, close } = await main.connect();
        for (const p of ["abs", "rel", "dl/secret", "chain/secret"])
            a.eq((await sftp.readFile(p).catch((e) => e)).code, "NO_SUCH_FILE", p);
        a.eq((await sftp.readdir("dl").catch((e) => e)).code, "NO_SUCH_FILE");
        a.eq(td.decode(await sftp.readFile("absin")), "inner\n", "an absolute link is read relative to the root");
        a.eq(await sftp.realpath("absin"), "/sub/inner.txt");
        a.eq(await sftp.readlink("abs"), "/outside/secret", "readlink returns the raw target");
        a.ok((await sftp.lstat("rel")).isSymbolicLink);
        const loop = await sftp.stat("loop1").catch((e) => e);
        a.eq(loop.code, "FAILURE", "a symlink loop fails");
        untouched(a);
        await close();
    });

    t.test("writes, renames, mkdirs and client-made links cannot escape", async (a) => {
        const { sftp, close } = await main.connect();
        a.ok((await sftp.writeFile("wl", "x").catch((e) => e)) instanceof Error, "write through an escaping link");
        a.ok((await sftp.writeFile("../outside/new2", "x").catch((e) => e)) instanceof Error);
        a.ok((await sftp.rename("hello.txt", "../outside/stolen").catch((e) => e)) instanceof Error);
        a.ok((await sftp.rename("hello.txt", "dl/stolen").catch((e) => e)) instanceof Error);
        await sftp.symlink("../../../../" + tree.outside.slice(1) + "/secret", "evil");
        a.eq((await sftp.readFile("evil").catch((e) => e)).code, "NO_SUCH_FILE");
        a.eq((await sftp.setstat("evil", { mode: 0o777 }).catch((e) => e)).code, "NO_SUCH_FILE");
        await sftp.remove("evil");
        await sftp.mkdir("../mk");
        a.ok(exists(R + "/mk") && !exists(tree.base + "/mk"), "mkdir ../mk lands in the root");
        await sftp.rmdir("mk");
        a.eq((await sftp.remove("..").catch((e) => e)).code, "FAILURE");
        a.ok(exists(R + "/hello.txt"));
        untouched(a);
        await close();
    });

    t.test("a final component swapped for a symlink after the check is never followed", async (a) => {
        const swap = (r) => {
            if (r.op === "open" && r.path.startsWith("/race")) {
                os.symlink(tree.outside + "/secret", R + "/race.tmp");
                os.rename(R + "/race.tmp", R + r.path);
            }
            return true;
        };
        const h = host({ root: R, allow: swap });
        const { sftp, close } = await h.connect();
        for (const n of ["race-r.txt", "race-w.txt"]) write(R + "/" + n, "plain\n");
        const rd = await sftp.readFile("race-r.txt").catch((e) => e);
        a.ok(rd instanceof Error, "read through the swapped link fails: " + (rd instanceof Error ? rd.code : td.decode(rd)));
        a.ok((await sftp.writeFile("race-w.txt", "PWNED").catch((e) => e)) instanceof Error, "write through the swapped link fails");
        untouched(a);
        for (const n of ["race-r.txt", "race-w.txt"]) os.remove(R + "/" + n);
        await close();
    });

    t.test("readOnly refuses writes and serves reads", async (a) => {
        const h = host({ readOnly: true });
        const { sftp, close } = await h.connect();
        a.eq(td.decode(await sftp.readFile("hello.txt")), "hello world\n");
        const tries = [sftp.writeFile("n.txt", "x"), sftp.remove("hello.txt"), sftp.mkdir("d"), sftp.rmdir("sub"), sftp.rename("hello.txt", "h2"),
            sftp.setstat("hello.txt", { mode: 0o600 }), sftp.symlink("hello.txt", "l2"), sftp.open("hello.txt", "r+"), sftp.open("hello.txt", "a")];
        for (const p of tries) a.eq((await p.catch((e) => e)).code, "PERMISSION_DENIED");
        a.ok(exists(R + "/hello.txt") && !exists(R + "/n.txt") && !exists(R + "/d"));
        await close();
        for (let i = 0; i < 100 && !h.served.length; i++) await sleep(20);
        a.eq(h.served[0].denied, 9);
    });

    t.test("allow hook: sync, resolved paths, target, user, write flag", async (a) => {
        const seen = [];
        const h = host({ root: R, allow: (r) => { seen.push(r); return !r.path.startsWith("/sub"); } });
        const { sftp, close } = await h.connect();
        os.symlink("hello.txt", R + "/hl2");
        a.eq(td.decode(await sftp.readFile("hl2")), "hello world\n");
        const o = seen.find((r) => r.op === "open");
        a.deep(o, { op: "open", path: "/hello.txt", user: "alice", write: false }, "path resolved through the link");
        a.eq((await sftp.readFile("sub/inner.txt").catch((e) => e)).code, "PERMISSION_DENIED");
        a.eq((await sftp.stat("absin").catch((e) => e)).code, "PERMISSION_DENIED", "a link into /sub is checked at its target");
        await sftp.writeFile("w.txt", "1");
        await sftp.rename("w.txt", "w2.txt");
        const rn = seen.find((r) => r.op === "rename");
        a.deep(rn, { op: "rename", path: "/w.txt", target: "/w2.txt", user: "alice", write: true });
        await sftp.remove("w2.txt");
        os.remove(R + "/hl2");
        await close();
    });

    t.test("allow hook: Promise results, throws and non-booleans", async (a) => {
        let mode = "async-yes";
        const h = host({ root: R, allow: (r) => {
            if (r.op !== "open") return true;
            if (mode === "async-yes") return sleep(30).then(() => true);
            if (mode === "async-no") return sleep(30).then(() => false);
            if (mode === "reject") return Promise.reject(new Error("x"));
            if (mode === "throw") throw new Error("boom");
            return 1;
        } });
        const { sftp, close } = await h.connect();
        const both = await Promise.all([sftp.readFile("hello.txt"), sftp.stat("hello.txt")]);
        a.eq(td.decode(both[0]), "hello world\n", "requests after a pending hook wait for it");
        mode = "async-no";
        a.eq((await sftp.readFile("hello.txt").catch((e) => e)).code, "PERMISSION_DENIED");
        for (const m of ["reject", "throw", "number"]) {
            mode = m;
            a.eq((await sftp.readFile("hello.txt").catch((e) => e)).code, "PERMISSION_DENIED", m);
        }
        mode = "async-yes";
        a.eq(td.decode(await sftp.readFile("hello.txt")), "hello world\n", "a decision never leaks into the next request");
        await close();
    });

    t.test("an async approval binds to the path it approved", async (a) => {
        os.mkdir(R + "/pub", 0o755);
        write(R + "/pub/x", "public\n");
        os.symlink("pub/x", R + "/sw");
        let release = null;
        const h = host({ root: R, allow: (r) => {
            if (r.op !== "open") return true;
            if (r.path === "/pub/x") return new Promise((res) => { release = res; });
            return !r.path.startsWith("/sub");
        } });
        const { sftp, close } = await h.connect();
        const p = sftp.readFile("sw").catch((e) => e);
        for (let i = 0; i < 100 && !release; i++) await sleep(10);
        a.ok(release !== null, "the hook is pending");
        os.remove(R + "/sw");
        os.symlink("sub/inner.txt", R + "/sw");
        release(true);
        const e = await p;
        a.eq(e.code, "PERMISSION_DENIED", "the approval of /pub/x does not open /sub/inner.txt");
        os.remove(R + "/sw");
        os.symlink("pub/x", R + "/sw");
        release = null;
        const q = sftp.readFile("sw");
        for (let i = 0; i < 100 && !release; i++) await sleep(10);
        release(true);
        a.eq(td.decode(await q), "public\n", "an unchanged path keeps its approval");
        await close();
        os.remove(R + "/sw");
        os.remove(R + "/pub/x");
        os.remove(R + "/pub");
    });

    t.test("no file descriptor leaks across denied, failed, paused and closed operations", async (a) => {
        const fds = () => os.readdir("/dev/fd")[0].length;
        const h = host({ root: R, allow: (r) => r.op === "open" ? sleep(5).then(() => !r.path.startsWith("/sub")) : !r.path.startsWith("/sub") });
        const warm = await h.connect();
        await warm.close();
        await sleep(100);
        const before = fds();
        for (let round = 0; round < 5; round++) {
            const { sftp, close } = await h.connect();
            const ops = [sftp.readFile("hello.txt"), sftp.readFile("sub/inner.txt"), sftp.stat("sub/../../x"), sftp.readFile("loop1"),
                sftp.readdir("dl"), sftp.readdir("sub"), sftp.open("hello.txt"), sftp.opendir("/"), sftp.readFile("rel"), sftp.lstat("chain/secret")];
            await Promise.all(ops.map((p) => p.catch((e) => e)));
            await close();
        }
        await sleep(200);
        a.eq(fds(), before, "open fds before and after five sessions (unclosed handles included)");
    });

    t.test("serve() misuse is a TypeError", async (a) => {
        const errs = [];
        const dir = tmpDir("sftpm");
        const hk = keygen(dir + "/host"), uk = keygen(dir + "/user");
        let mode = "late";
        const srv = SSHServer.listen({ hostKey: hk.priv, authorizedKeys: uk.pub, onSession: async (s) => {
            try {
                if (mode === "late") { await sleep(50); return await SFTPServer.serve(s, { root: R }); }
                if (mode === "bogus") return await SFTPServer.serve(s, { root: R, bogus: 1 });
                if (mode === "file") return await SFTPServer.serve(s, { root: R + "/hello.txt" });
                if (mode === "meta") return await SFTPServer.serve(s, { root: R, dyna: { maxHandles: 0 } });
            } catch (e) { errs.push(e); throw e; }
        } });
        hosts.push({ close: () => srv.close() });
        const kh = dir + "/kh";
        write(kh, "[127.0.0.1]:" + srv.port + " " + pubBlob(hk.pub) + "\n");
        for (const m of ["late", "bogus", "file", "meta"]) {
            mode = m;
            const ssh = await SSHClient.connect({ host: "127.0.0.1", port: srv.port, username: "u", privateKey: uk.priv, knownHosts: kh });
            const e = await SFTPClient.open(ssh).catch((x) => x);
            a.ok(e instanceof Error, m + ": the client sees the session end");
            ssh.close();
        }
        a.eq(errs.length, 4);
        a.ok(errs[0] instanceof TypeError && /synchronously/.test(errs[0].message), String(errs[0]));
        a.match(errs[1].message, /bogus/);
        a.ok(errs[2] instanceof TypeError && /root/.test(errs[2].message));
        a.ok(errs[3] instanceof RangeError);
    });
}

await t.run();
