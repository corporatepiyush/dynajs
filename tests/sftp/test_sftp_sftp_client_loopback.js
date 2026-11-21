// flags: --std
// covers: integration dyna:sftp.SFTPClient dyna:sftp.SFTPServer -- this module's SFTP client against this module's SFTP server over dyna:ssh on 127.0.0.1: every v3 operation, pipelined streaming, short-read gap filling, errors, caps
// timeout: 180
// The server is SFTPServer.serve() on an SSHServer "sftp" subsystem session, rooted in a temp directory. Covered: realpath, stat,
// lstat, setstat (mode, times, size), mkdir/rmdir, remove, rename (refused onto an existing target), symlink/readlink (OpenSSH
// argument order: target first), readdir and opendir/read/close, open flags r/w/wx/a/r+, fstat/fsetstat (fsetstat only on a handle opened for writing), setstat mode 0 then back by path, pread/pwrite, 8 MiB
// readFile/writeFile and SFTPFile.read()/write() streams byte-exact with more than 8 requests in flight, short server reads
// (dyna.maxReadBytes 4096 under 32 KiB client chunks) filled in order, status codes on the error object (NO_SUCH_FILE,
// FAILURE, PERMISSION_DENIED), dyna.maxHandles, strict option bags, the constructor refusing `new`, close() rejecting pending
// requests, and the serve() result counters.
import * as os from "os";
import { SFTPClient, SFTPServer } from "dyna:sftp";
import { SSHClient } from "dyna:ssh";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir } from "../ssh/ssh_util.js";
import { bytes, same, readBin, writeBin, exists, makeTree, sftpHost } from "./sftp_util.js";

const t = suite("sftp_client_loopback");
const hosts = [];
t.after(() => { for (const h of hosts) h.close(); });
const te = new TextEncoder(), td = new TextDecoder();

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const tree = makeTree("sftploop");
    const host = (opts = {}, extra) => { const h = sftpHost(tmpDir("sftph"), { root: tree.root, ...opts }, extra); hosts.push(h); return h; };
    const main = host();

    t.test("realpath, stat, lstat, readdir, opendir", async (a) => {
        const { sftp, close } = await main.connect();
        a.eq(sftp.version, 3);
        a.eq(await sftp.realpath("."), "/");
        a.eq(await sftp.realpath("sub/../sub/./"), "/sub");
        a.eq(await sftp.realpath("/nope"), "/nope", "a missing last component is allowed");
        const st = await sftp.stat("hello.txt");
        a.eq(st.size, 12);
        a.ok(st.isFile && !st.isDirectory && !st.isSymbolicLink);
        a.eq(st.mode & 0o777, os.stat(tree.root + "/hello.txt")[0].mode & 0o777);
        os.symlink("hello.txt", tree.root + "/hl");
        a.ok((await sftp.lstat("hl")).isSymbolicLink);
        a.ok((await sftp.stat("hl")).isFile);
        const names = (await sftp.readdir("/")).map((e) => e.filename).sort();
        a.deep(names.filter((n) => n !== "." && n !== ".."), ["hello.txt", "hl", "sub"].sort());
        const e = (await sftp.readdir("/")).find((x) => x.filename === "sub");
        a.ok(e.attrs.isDirectory);
        a.match(e.longname, /^d[rwx-]{9} .* sub$/);
        const d = await sftp.opendir("sub");
        let all = [];
        for (let b; (b = await d.read()) !== null;) all = all.concat(b.map((x) => x.filename));
        await d.close();
        a.ok(all.includes("inner.txt"));
        await close();
    });

    t.test("mkdir, rmdir, remove, rename, symlink, readlink, setstat", async (a) => {
        const { sftp, close } = await main.connect();
        await sftp.mkdir("newdir", { mode: 0o750 });
        a.eq(os.stat(tree.root + "/newdir")[0].mode & 0o777, 0o750);
        await sftp.writeFile("newdir/f", "abc");
        await sftp.rename("newdir/f", "newdir/g");
        a.ok(!exists(tree.root + "/newdir/f") && exists(tree.root + "/newdir/g"));
        await sftp.writeFile("newdir/h", "x");
        const err = await sftp.rename("newdir/g", "newdir/h").catch((e) => e);
        a.eq(err.code, "FAILURE", "v3 rename refuses an existing target");
        await sftp.symlink("g", "newdir/link");
        a.eq(os.readlink(tree.root + "/newdir/link")[0], "g", "OpenSSH order: target first, link path second");
        a.eq(await sftp.readlink("newdir/link"), "g");
        a.eq(td.decode(await sftp.readFile("newdir/link")), "abc");
        await sftp.setstat("newdir/g", { mode: 0 });
        a.eq(os.stat(tree.root + "/newdir/g")[0].mode & 0o777, 0, "mode 0 is set by path");
        await sftp.setstat("newdir/g", { mode: 0o600, atime: 1000000, mtime: 2000000, size: 2 });
        const st = os.stat(tree.root + "/newdir/g")[0];
        a.eq(st.mode & 0o777, 0o600);
        a.eq(Math.floor(st.mtime / 1000), 2000000);
        a.eq(st.size, 2);
        for (const f of ["g", "h", "link"]) await sftp.remove("newdir/" + f);
        await sftp.rmdir("newdir");
        a.ok(!exists(tree.root + "/newdir"));
        await close();
    });

    t.test("errors carry SFTP status codes", async (a) => {
        const { sftp, close } = await main.connect();
        a.eq((await sftp.stat("missing").catch((e) => e)).code, "NO_SUCH_FILE");
        a.eq((await sftp.open("sub").catch((e) => e)).code, "FAILURE", "a directory is not a file");
        a.eq((await sftp.open("hello.txt", "wx").catch((e) => e)).code, "FAILURE");
        a.eq((await sftp.rmdir("hello.txt").catch((e) => e)).code, "NO_SUCH_FILE");
        a.eq((await sftp.remove("sub").catch((e) => e)).status > 0, true);
        const e = await sftp.readFile("nope/x").catch((x) => x);
        a.match(e.message, /^SFTP: /);
        a.eq(e.code, "NO_SUCH_FILE");
        await close();
    });

    t.test("8 MiB writeFile/readFile and streams, byte-exact and pipelined", async (a) => {
        const { sftp, close } = await main.connect();
        const data = bytes(8 << 20, 3);
        await sftp.writeFile("big.bin", data);
        a.ok(same(readBin(tree.root + "/big.bin"), data), "writeFile lands byte-exact");
        a.ok(sftp.stats.maxInflight > 8, "writes pipelined: " + sftp.stats.maxInflight);
        const back = await sftp.readFile("big.bin");
        a.ok(same(back, data), "readFile byte-exact");
        const f = await sftp.open("big2.bin", "w");
        for (let o = 0; o < data.length; o += 100000) await f.write(data.subarray(o, Math.min(data.length, o + 100000)));
        a.eq(f.writePosition, data.length);
        await f.close();
        a.ok(same(readBin(tree.root + "/big2.bin"), data), "write() stream byte-exact");
        const r = await sftp.open("big2.bin");
        const parts = [];
        let n = 0;
        for (let c; (c = await r.read()) !== null;) { parts.push(c); n += c.byteLength; }
        await r.close();
        const all = new Uint8Array(n);
        let o = 0;
        for (const p of parts) { all.set(p, o); o += p.byteLength; }
        a.ok(same(all, data), "read() stream byte-exact");
        a.ok(sftp.stats.maxInflight >= 32, "reads pipelined: " + sftp.stats.maxInflight);
        await sftp.remove("big.bin");
        await sftp.remove("big2.bin");
        await close();
    }, { timeoutMs: 60000 });

    t.test("short server reads are filled in order (maxReadBytes 4096, client chunks 32 KiB)", async (a) => {
        const h = host({ dyna: { maxReadBytes: 4096 } });
        const { sftp, close } = await h.connect({ dyna: { chunkBytes: 32768, maxInflight: 16 } });
        const data = bytes((1 << 20) + 777, 9);
        writeBin(tree.root + "/short.bin", data);
        a.ok(same(await sftp.readFile("short.bin"), data), "readFile");
        const f = await sftp.open("short.bin");
        const parts = [];
        for (let c; (c = await f.read()) !== null;) { a.ok(c.byteLength <= 4096); parts.push(c); }
        await f.close();
        let n = 0;
        for (const p of parts) n += p.byteLength;
        const all = new Uint8Array(n);
        let o = 0;
        for (const p of parts) { all.set(p, o); o += p.byteLength; }
        a.ok(same(all, data), "read() stream");
        os.remove(tree.root + "/short.bin");
        await close();
    }, { timeoutMs: 60000 });

    t.test("open flags, pread/pwrite, append, fstat/fsetstat", async (a) => {
        const { sftp, close } = await main.connect();
        const f = await sftp.open("p.bin", "w+");
        await f.pwrite(10, te.encode("world"));
        await f.pwrite(0, te.encode("hello"));
        a.eq(td.decode(await f.pread(0, 5)), "hello");
        a.eq(await f.pread(100, 10), null, "EOF is null");
        a.eq((await f.stat()).size, 15);
        await f.setstat({ size: 5 });
        a.eq((await f.stat()).size, 5);
        await f.close();
        a.eq((await f.close().catch((e) => e)).code, "FAILURE", "double close rejects");
        const ap = await sftp.open("p.bin", "a");
        await ap.write("++");
        await ap.close();
        a.eq(td.decode(await sftp.readFile("p.bin")), "hello++");
        const ro = await sftp.open("p.bin", "r");
        a.ok((await ro.pwrite(0, "x").catch((e) => e)).code !== undefined, "a read-only handle refuses writes");
        a.eq((await ro.setstat({ mode: 0o600 }).catch((e) => e)).code, "PERMISSION_DENIED", "fsetstat needs a handle opened for writing");
        await ro.close();
        await sftp.remove("p.bin");
        await close();
    });

    t.test("dyna.maxHandles caps open handles", async (a) => {
        const h = host({ dyna: { maxHandles: 2 } });
        const { sftp, close } = await h.connect();
        const f1 = await sftp.open("hello.txt"), f2 = await sftp.open("hello.txt");
        const e = await sftp.open("hello.txt").catch((x) => x);
        a.eq(e.code, "FAILURE");
        a.match(e.message, /too many open handles/);
        await f1.close();
        const f3 = await sftp.open("hello.txt");
        await f2.close();
        await f3.close();
        await close();
    });

    t.test("strict options, constructor, argument errors", async (a) => {
        a.throws(() => new SFTPClient(), TypeError);
        const { ssh, sftp, close } = await main.connect();
        a.throws(() => SFTPClient.open(ssh, { bogus: 1 }), /bogus/);
        a.throws(() => SFTPClient.open(ssh, { dyna: { bogus: 1 } }), /bogus/);
        a.throws(() => SFTPClient.open(ssh, { dyna: { maxInflight: 0 } }), RangeError);
        a.throws(() => SFTPClient.open(ssh, { dyna: { chunkBytes: 1 << 20 } }), RangeError);
        a.throws(() => SFTPClient.open({}), TypeError);
        a.throws(() => sftp.open("x", "rw"), TypeError);
        a.throws(() => sftp.stat(1), TypeError);
        a.throws(() => sftp.setstat("x", { mode: -1 }), RangeError);
        a.throws(() => sftp.setstat("x", { uid: 1 }), TypeError);
        a.throws(() => sftp.setstat("x", { bogus: 1 }), /bogus/);
        a.throws(() => SFTPServer.serve({}, { root: "/" }), TypeError);
        await close();
    });

    t.test("close() rejects pending requests and later calls", async (a) => {
        const { ssh, sftp } = await main.connect();
        const p = sftp.readFile("hello.txt").catch((x) => x);
        await sftp.close();
        const e = await p;
        a.ok(e instanceof Error, "pending request rejected");
        a.eq((await sftp.stat("hello.txt").catch((x) => x)).code, "NO_CONNECTION");
        ssh.close();
    });

    t.test("serve() resolves with counters when the client leaves", async (a) => {
        const h = host();
        const { sftp, close } = await h.connect();
        await sftp.writeFile("cnt.bin", bytes(100000, 1));
        await sftp.readFile("cnt.bin");
        await sftp.remove("cnt.bin");
        await close();
        for (let i = 0; i < 100 && !h.served.length; i++) await new Promise((r) => setTimeout(r, 20));
        a.eq(h.served.length, 1);
        const st = h.served[0];
        a.eq(st.bytesWritten, 100000);
        a.eq(st.bytesRead, 100000);
        a.ok(st.requests > 5);
        a.eq(st.error, null);
    });

    t.test("an SSHClient that is not connected, and SFTP over a closed SSH connection", async (a) => {
        const { ssh, sftp } = await main.connect();
        ssh.close();
        const e = await sftp.stat("hello.txt").catch((x) => x);
        a.ok(e instanceof Error);
        a.throws(() => SFTPClient.open(ssh), TypeError, "a closed SSHClient is refused synchronously");
    });

    t.test("SFTPClient is not a DynResource: borrowed DynResource accessors refuse it, borrowed dispose leaves it working", async (a) => {
        const { sftp, close } = await main.connect();
        const closedGet = Object.getOwnPropertyDescriptor(SSHClient.prototype, "closed").get;
        a.throws(() => closedGet.call(sftp), TypeError, "closed read off an SFTPClient is refused, not read from a foreign struct");
        a.eq(SSHClient.prototype.dispose.call(sftp), undefined);
        a.ok((await sftp.stat("hello.txt")).isFile, "the session still works");
        await close();
    });
}

await t.run();
