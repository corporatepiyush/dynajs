// Shared harness for the dyna:sftp suites. It builds on tests/ssh/ssh_util.js (keys made by ssh-keygen at run time under TMPDIR,
// a private OpenSSH sshd whose stop() kills the daemon through its pid file). sftpHost() starts this module's SSHServer with an
// onSession that hands "sftp" subsystem sessions to SFTPServer.serve(session, opts) and records each serve() result in `served`;
// a sibling directory named "outside" holds a secret that no chroot escape may reach. Binary file helpers read and write
// through std so the comparisons never go through the code under test.
import * as std from "std";
import * as os from "os";
import { SSHServer, SSHClient } from "dyna:ssh";
import { SFTPServer, SFTPClient } from "dyna:sftp";
import { tmpDir, keygen, write, pubBlob } from "../ssh/ssh_util.js";

export function bytes(n, seed) {
    const u = new Uint8Array(n);
    let x = (seed * 2654435761) >>> 0 || 1;
    for (let i = 0; i < n; i++) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; u[i] = x & 255; }
    return u;
}

export function same(a, b) {
    if (!a || !b || a.byteLength !== b.byteLength) return false;
    for (let i = 0; i < a.byteLength; i++) if (a[i] !== b[i]) return false;
    return true;
}

export function writeBin(path, u8) {
    const f = std.open(path, "wb");
    if (!f) throw new Error("cannot write " + path);
    f.write(u8.buffer, u8.byteOffset, u8.byteLength);
    f.close();
}

export function readBin(path) {
    const f = std.open(path, "rb");
    if (!f) return null;
    f.seek(0, std.SEEK_END);
    const n = f.tell();
    f.seek(0, std.SEEK_SET);
    const u = new Uint8Array(n);
    f.read(u.buffer, 0, n);
    f.close();
    return u;
}

export function exists(path) {
    const [, err] = os.lstat(path);
    return err === 0;
}

export function makeTree(tag) {
    const base = tmpDir(tag);
    const root = base + "/root";
    const outside = base + "/outside";
    os.mkdir(root, 0o755);
    os.mkdir(outside, 0o755);
    write(outside + "/secret", "TOP-SECRET\n");
    write(root + "/hello.txt", "hello world\n");
    os.mkdir(root + "/sub", 0o755);
    write(root + "/sub/inner.txt", "inner\n");
    return { base, root, outside };
}

export function sftpHost(dir, sftpOpts, extra = {}) {
    const host = keygen(dir + "/host");
    const user = keygen(dir + "/user");
    const served = [];
    const errors = [];
    const opts = typeof sftpOpts === "function" ? sftpOpts : () => sftpOpts;
    const srv = SSHServer.listen({
        hostKey: host.priv, authorizedKeys: user.pub + "\n",
        onSession: (s) => {
            if (s.type !== "subsystem" || s.command !== "sftp") { s.exit(1); return; }
            const p = SFTPServer.serve(s, opts(s));
            p.then((st) => served.push(st), (e) => errors.push(e));
            return p;
        },
        ...extra,
    });
    const kh = dir + "/kh" + srv.port;
    write(kh, "[127.0.0.1]:" + srv.port + " " + pubBlob(host.pub) + "\n");
    return {
        srv, host, user, served, errors, kh,
        base: { host: "127.0.0.1", port: srv.port, username: "alice", privateKey: user.priv, knownHosts: kh },
        async connect(sftpOptions) {
            const ssh = await SSHClient.connect(this.base);
            const sftp = await SFTPClient.open(ssh, sftpOptions);
            return { ssh, sftp, async close() { await sftp.close(); ssh.close(); } };
        },
        close() { try { srv.close(); } catch {} },
    };
}
