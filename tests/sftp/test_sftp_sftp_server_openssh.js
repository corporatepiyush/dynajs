// flags: --std
// covers: dyna:sftp.SFTPServer -- interop: the OpenSSH sftp client (batch mode) against this module's SFTP server
// timeout: 120
// A 4 MiB put and get round trip byte-exact; pwd, ls -l, mkdir, rename, ln -s (OpenSSH SYMLINK argument order), chmod, rm, rmdir;
// "cd .." at the root stays at "/"; gets of ../outside/secret and /../outside/secret fail (the "-" prefix lets the batch go on)
// and leave no local copy. The suite SKIPs without an OpenSSH >= 9.9 ssh/sftp client (mlkem768x25519-sha256 is our default).
import * as os from "os";
import * as sys from "dyna:sys";
import { SFTPServer } from "dyna:sftp";
import { suite } from "../lib/kit.js";
import { HAVE_SSH, SKIP_WHY_SSH, tmpDir, write, collect } from "../ssh/ssh_util.js";
import { bytes, same, readBin, writeBin, exists, makeTree, sftpHost } from "./sftp_util.js";

const t = suite("sftp_server_openssh");
const hosts = [];
t.after(() => { for (const h of hosts) h.close(); });

async function sftpBatch(h, dir, lines) {
    write(dir + "/batch", lines.join("\n") + "\n");
    const p = new sys.Spawn("sftp", ["-F", "/dev/null", "-b", dir + "/batch", "-P", String(h.srv.port), "-i", dir + "/user",
        "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + h.kh, "-o", "StrictHostKeyChecking=yes",
        "-o", "LogLevel=ERROR", "alice@127.0.0.1"], { timeoutMs: 60000 });
    try {
        const [out, err] = await Promise.all([collect(p.stdout), collect(p.stderr)]);
        const r = await p.wait();
        return { code: r.code, out: new TextDecoder().decode(out), err: new TextDecoder().decode(err) };
    } finally { p.close(); }
}

if (!HAVE_SSH || !sys.Which("sftp")) t.skip("sftp", HAVE_SSH ? "no sftp client on this host" : SKIP_WHY_SSH);
else {
    const tree = makeTree("sftposs");
    const R = tree.root;

    t.test("OpenSSH sftp: put/get, ls, mkdir, rename, ln -s, chmod, rm, rmdir, no escape", async (a) => {
        const dir = tmpDir("sftpoc");
        a.eq(typeof SFTPServer.serve, "function");
        const h = sftpHost(dir, { root: R });
        hosts.push(h);
        const L = dir + "/local";
        os.mkdir(L, 0o755);
        const data = bytes(4 << 20, 5);
        writeBin(L + "/up.bin", data);
        const r = await sftpBatch(h, dir, [
            "pwd", "put " + L + "/up.bin up.bin", "get up.bin " + L + "/down.bin", "ls -l", "mkdir d1", "rename up.bin d1/up2.bin",
            "ln -s up2.bin d1/lnk", "chmod 600 d1/up2.bin", "ls -l d1", "-get ../outside/secret " + L + "/leak",
            "-get /../outside/secret " + L + "/leak2", "-get d1/../../outside/secret " + L + "/leak3", "cd ..", "pwd",
        ]);
        a.eq(r.code, 0, "batch ok: " + r.err);
        a.ok(same(readBin(L + "/down.bin"), data), "get is byte-exact");
        a.ok(same(readBin(R + "/d1/up2.bin"), data), "put landed byte-exact (then renamed)");
        a.eq((r.out.match(/Remote working directory: \/\n/g) || []).length, 2, "pwd is / before and after cd ..");
        a.match(r.out, /hello\.txt/);
        a.match(r.out, /lnk/);
        a.eq(os.readlink(R + "/d1/lnk")[0], "up2.bin");
        a.eq(os.stat(R + "/d1/up2.bin")[0].mode & 0o777, 0o600);
        a.ok(!exists(L + "/leak") && !exists(L + "/leak2") && !exists(L + "/leak3"), "no escape copied a file");
        a.match(r.err, /not found|No such file/i);
        const r2 = await sftpBatch(h, dir, ["rm d1/lnk", "rm d1/up2.bin", "rmdir d1", "ls"]);
        a.eq(r2.code, 0, r2.err);
        a.ok(!exists(R + "/d1"));
        for (let i = 0; i < 100 && h.served.length < 2; i++) await new Promise((res) => setTimeout(res, 20));
        a.eq(h.served.length, 2);
        a.eq(h.served[0].bytesWritten, data.length);
        a.eq(h.served[0].bytesRead, data.length);
    }, { timeoutMs: 90000 });
}

await t.run();
