import * as std from "std";
import { SSHServer, SSHClient } from "dyna:ssh";
import { SFTPServer, SFTPClient } from "dyna:sftp";
import { tmpDir, keygen, write, pubBlob } from "../ssh/ssh_util.js";
import { bytes, makeTree } from "./sftp_util.js";

const mode = scriptArgs[1];
const tree = makeTree("sftptd");
const dir = tmpDir("sftptd");
const hk = keygen(dir + "/host"), uk = keygen(dir + "/user");
write(tree.root + "/hello.txt", "hello\n");
const srv = SSHServer.listen({ hostKey: hk.priv, authorizedKeys: uk.pub, onSession: (s) => SFTPServer.serve(s, {
    root: tree.root, allow: (r) => mode === "hook-pending-throw" && r.op === "open" ? new Promise(() => {}) : true,
}) });
write(dir + "/kh", "[127.0.0.1]:" + srv.port + " " + pubBlob(hk.pub) + "\n");
const ssh = await SSHClient.connect({ host: "127.0.0.1", port: srv.port, username: "u", privateKey: uk.priv, knownHosts: dir + "/kh" });
const sftp = await SFTPClient.open(ssh);
console.log("sftp-open");
if (mode === "client-throw") throw new Error("teardown-boom");
if (mode === "client-exit") std.exit(3);
if (mode === "read-pending-throw") {
    await sftp.writeFile("big.bin", bytes(4 << 20, 2));
    const f = await sftp.open("big.bin");
    f.read().then(() => {}, () => {});
    sftp.readFile("big.bin").catch(() => {});
    throw new Error("teardown-boom");
}
if (mode === "hook-pending-throw") {
    sftp.readFile("hello.txt").catch(() => {});
    await new Promise((r) => setTimeout(r, 100));
    throw new Error("teardown-boom");
}
if (mode === "server-close" || mode === "ssh-close") {
    const p = sftp.writeFile("w.bin", bytes(16 << 20, 4)).catch((e) => e);
    const q = sftp.readFile("hello.txt").catch((e) => e);
    await new Promise((r) => setTimeout(r, 30));
    if (mode === "server-close") srv.close();
    else ssh.close();
    const [e1] = await Promise.all([p, q]);
    console.log("transfer ended: " + (e1 instanceof Error ? e1.code : "ok"));
    if (mode === "ssh-close") srv.close();
}
