import { SSHClient, SSHServer } from "dyna:ssh";
import * as std from "std";
import { TCPServer } from "dyna:net";
import { tmpDir, keygen, write, pubBlob } from "./ssh_util.js";
import { echoTarget, burstTarget } from "./fwd_util.js";

const mode = scriptArgs[1];
const dir = tmpDir("fwdteardown");
const host = keygen(dir + "/host");
const user = keygen(dir + "/user");
const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub + "\n", forward: () => true, onSession: async () => { await new Promise(() => {}); } });
write(dir + "/kh", "[127.0.0.1]:" + s.port + " " + pubBlob(host.pub) + "\n");
const opts = { host: "127.0.0.1", port: s.port, username: "u", privateKey: user.priv, knownHosts: dir + "/kh" };
const echo = echoTarget();
const burst = burstTarget(8 << 20);
const c = await SSHClient.connect(opts);
const hold = (port) => new Promise((r) => TCPServer.connect({ host: "127.0.0.1", port }, { connect: (conn) => { conn.write(new Uint8Array(1 << 20)); r(conn); } }));
if (mode === "local-live") {
    const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
    await hold(f.listenPort);
    await hold(c.forwardLocal({ host: "127.0.0.1", port: burst.port }).listenPort);
} else if (mode === "remote-live") {
    const f = await c.forwardRemote({ remotePort: 0, host: "127.0.0.1", port: echo.port });
    await hold(f.listenPort);
    c.forwardRemote({ remotePort: 0, host: "127.0.0.1", port: echo.port }).catch(() => {});
} else if (mode === "churn-clean") {
    const r = await c.forwardRemote({ remotePort: 0, host: "127.0.0.1", port: echo.port });
    const inner = await SSHClient.connect({ ...opts, via: c });
    for (let i = 0; i < 40; i++) {
        const f = (i & 1 ? inner : c).forwardLocal({ host: "127.0.0.1", port: echo.port });
        for (const port of [f.listenPort, r.listenPort]) {
            await new Promise((res) => TCPServer.connect({ host: "127.0.0.1", port }, {
                connect: (conn) => conn.write(new Uint8Array(4096)),
                data: (conn, b) => conn.close(),
                close: () => res(),
            }));
        }
        f.close();
    }
    r.close();
    inner.close();
    c.close();
    s.close();
    echo.close();
    burst.close();
    console.log("forwards-live");
    std.exit(0);
} else if (mode === "via-live") {
    const inner = await SSHClient.connect({ ...opts, via: c });
    const x = await inner.exec("hang");
    x.stdout.getReader().read();
    inner.forwardLocal({ host: "127.0.0.1", port: echo.port });
    SSHClient.connect({ ...opts, via: inner }).catch(() => {});
}
await new Promise((r) => setTimeout(r, 100));
console.log("forwards-live");
if (std.getenv("FWD_TEARDOWN_EXIT")) std.exit(3);
throw new Error("teardown-boom");
