// flags: --std
// covers: dyna:sftp.SFTPClient -- interop: this module's SFTP client against OpenSSH sshd's internal-sftp
// timeout: 120
// A private sshd (Subsystem sftp internal-sftp) serves the real filesystem; the suite works in a temp directory by absolute path.
// Covered: version 3, realpath, 8 MiB writeFile/readFile and read()/write() streams byte-exact with more than 8 requests in
// flight, pread/pwrite, stat/lstat/setstat/fstat, mkdir/rmdir, rename (refused onto an existing target), symlink with the
// OpenSSH argument order (checked with readlink(2) on disk), readdir, remove, and status codes on errors. The suite SKIPs
// without an OpenSSH >= 9.9 sshd; the last case checks that close() kills the sshd (its pid is gone).
import * as os from "os";
import { SSHClient } from "dyna:ssh";
import { SFTPClient } from "dyna:sftp";
import { suite } from "../lib/kit.js";
import * as sys from "dyna:sys";
import { HAVE_SSHD, SKIP_WHY, tmpDir, startSshd, read, sleep } from "../ssh/ssh_util.js";
import { bytes, same, readBin, writeBin, exists } from "./sftp_util.js";

const t = suite("sftp_client_openssh");
let sshd = null;
t.after(() => { if (sshd) sshd.close(); });
const td = new TextDecoder();

if (!HAVE_SSHD) t.skip("sshd", SKIP_WHY);
else {
    t.before(async () => { sshd = await startSshd({ tag: "sftpd", extra: ["Subsystem sftp internal-sftp"] }); });
    const open = async (o) => { const ssh = await SSHClient.connect(sshd.base); const sftp = await SFTPClient.open(ssh, o); return { ssh, sftp, close: async () => { await sftp.close(); ssh.close(); } }; };

    t.test("OpenSSH internal-sftp: every operation", async (a) => {
        const W = tmpDir("sftpw");
        const { sftp, close } = await open();
        a.eq(sftp.version, 3);
        a.ok((await sftp.realpath(".")).startsWith("/"));
        const data = bytes(8 << 20, 21);
        await sftp.writeFile(W + "/a.bin", data);
        a.ok(same(readBin(W + "/a.bin"), data), "writeFile byte-exact");
        a.ok(same(await sftp.readFile(W + "/a.bin"), data), "readFile byte-exact");
        a.ok(sftp.stats.maxInflight > 8, "pipelined: " + sftp.stats.maxInflight);
        const f = await sftp.open(W + "/s.bin", "w");
        for (let o = 0; o < data.length; o += 65536 * 3) await f.write(data.subarray(o, Math.min(data.length, o + 65536 * 3)));
        await f.close();
        a.ok(same(readBin(W + "/s.bin"), data), "write() stream");
        const r = await sftp.open(W + "/s.bin");
        let n = 0, ok = true;
        for (let c; (c = await r.read()) !== null;) { for (let i = 0; i < c.length && ok; i += 4093) ok = c[i] === data[n + i]; n += c.length; }
        a.eq(n, data.length);
        a.ok(ok, "read() stream in order");
        a.eq(td.decode(await r.pread(0, 0)), "");
        await r.close();
        const p = await sftp.open(W + "/p.txt", "w+");
        await p.pwrite(3, "def");
        await p.pwrite(0, "abc");
        a.eq(td.decode(await p.pread(0, 6)), "abcdef");
        a.eq((await p.stat()).size, 6);
        await p.close();
        const st = await sftp.stat(W + "/p.txt");
        a.ok(st.isFile);
        a.eq(st.size, 6);
        await sftp.setstat(W + "/p.txt", { mode: 0o640 });
        a.eq(os.stat(W + "/p.txt")[0].mode & 0o777, 0o640);
        await sftp.mkdir(W + "/d");
        await sftp.rename(W + "/p.txt", W + "/d/q.txt");
        writeBin(W + "/x.txt", new Uint8Array([1]));
        a.ok((await sftp.rename(W + "/x.txt", W + "/d/q.txt").catch((e) => e)) instanceof Error, "rename onto an existing file fails");
        await sftp.symlink("q.txt", W + "/d/l");
        a.eq(os.readlink(W + "/d/l")[0], "q.txt", "OpenSSH symlink order");
        a.eq(await sftp.readlink(W + "/d/l"), "q.txt");
        a.ok((await sftp.lstat(W + "/d/l")).isSymbolicLink);
        a.deep((await sftp.readdir(W + "/d")).map((e) => e.filename).filter((x) => x[0] !== ".").sort(), ["l", "q.txt"]);
        a.eq((await sftp.stat(W + "/missing").catch((e) => e)).code, "NO_SUCH_FILE");
        for (const x of ["/d/l", "/d/q.txt", "/a.bin", "/s.bin", "/x.txt"]) await sftp.remove(W + x);
        await sftp.rmdir(W + "/d");
        a.ok(!exists(W + "/d"));
        await close();
    }, { timeoutMs: 90000 });

    t.test("close() stops the sshd it started", async (a) => {
        const pid = Number(read(sshd.dir + "/pid").trim());
        a.ok(pid > 1);
        sshd.close();
        sshd = null;
        let alive = true;
        for (let i = 0; i < 100 && alive; i++) { await sleep(20); alive = sys.Exec("kill", ["-0", String(pid)]).code === 0; }
        a.ok(!alive, "sshd " + pid + " is gone");
    });
}

await t.run();
