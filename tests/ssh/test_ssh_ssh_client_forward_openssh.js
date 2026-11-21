// flags: --std
// covers: integration dyna:ssh.SSHClient.forwardLocal dyna:ssh.SSHClient.forwardRemote dyna:ssh.SSHClient.connect -- interop: this module's client forwarding and ProxyJump against OpenSSH sshd
// timeout: 180
// forwardLocal through sshd: 4 MiB byte-exact, a half-close answered after EOF, a slow reader gets a 6 MiB burst whole. forwardRemote
// with port 0: sshd allocates the port and its reply carries it, 2 MiB byte-exact back through forwarded-tcpip, close() sends
// cancel-tcpip-forward and sshd stops listening. An sshd with AllowTcpForwarding no: forwardRemote rejects, a local forward's
// connection is closed with no bytes. ProxyJump (connect({via})): through sshd to the same sshd, and through sshd to this module's
// server (its own host key checked). The sshd started here is stopped in teardown. The suite SKIPs without OpenSSH sshd >= 9.9.
import { SSHClient, SSHServer } from "dyna:ssh";
import { suite } from "../lib/kit.js";
import { HAVE_SSHD, SKIP_WHY, startSshd, keygen, write, pubBlob, run, sleep } from "./ssh_util.js";
import { echoTarget, halfTarget, burstTarget, exchange, text } from "./fwd_util.js";

const t = suite("ssh_client_forward_openssh");
const closers = [];
t.after(() => { for (const s of closers) { try { s.close(); } catch {} } });

function bytes(n, seed) { const u = new Uint8Array(n); let x = seed >>> 0 || 1; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; u[i] = x >>> 24; } return u; }
function same(a, b) { if (a.byteLength !== b.byteLength) return false; for (let i = 0; i < a.byteLength; i++) if (a[i] !== b[i]) return false; return true; }

if (!HAVE_SSHD) t.skip("sshd", SKIP_WHY);
else {
    const keep = (x) => { closers.push(x); return x; };
    const echo = keep(echoTarget());
    const half = keep(halfTarget());
    const burst = keep(burstTarget(6 << 20));
    let d = null;
    const sshd = async () => d || (d = keep(await startSshd({ tag: "cfwdossh", extra: ["AllowTcpForwarding yes", "GatewayPorts no"] })));

    t.test("forwardLocal through sshd", async (a) => {
        const s = await sshd();
        const c = await SSHClient.connect(s.base);
        const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
        const data = bytes(4 << 20, 11);
        const r = await exchange(f.listenPort, data);
        a.ok(same(r.bytes, data), "4 MiB byte-exact through sshd direct-tcpip: " + r.bytes.byteLength);
        const f2 = c.forwardLocal({ host: "127.0.0.1", port: half.port });
        a.eq(text((await exchange(f2.listenPort, bytes(300000, 2))).bytes), "got 300000", "half-close through sshd");
        const f3 = c.forwardLocal({ host: "127.0.0.1", port: burst.port });
        const b = await exchange(f3.listenPort, new Uint8Array(0), { slowMs: 2, timeoutMs: 60000 });
        a.eq(b.bytes.byteLength, 6 << 20, "slow reader gets the whole burst");
        f.close(); f2.close(); f3.close();
        c.close();
    }, { timeoutMs: 90000 });

    t.test("forwardRemote through sshd: port 0, bytes, cancel", async (a) => {
        const s = await sshd();
        const c = await SSHClient.connect(s.base);
        const f = await c.forwardRemote({ remoteHost: "127.0.0.1", remotePort: 0, host: "127.0.0.1", port: echo.port });
        a.eq(f.type, "remote");
        a.ok(f.listenPort > 0, "sshd allocated " + f.listenPort);
        const data = bytes(2 << 20, 12);
        const r = await exchange(f.listenPort, data);
        a.ok(same(r.bytes, data), "2 MiB byte-exact through sshd forwarded-tcpip: " + r.bytes.byteLength);
        const port = f.listenPort;
        f.close();
        let gone = false;
        for (let i = 0; i < 100 && !gone; i++) { const x = await exchange(port, bytes(4, 1), { timeoutMs: 2000 }); gone = x.bytes.byteLength === 0; if (!gone) await sleep(30); }
        a.ok(gone, "sshd stopped listening after cancel-tcpip-forward");
        const r2 = await exec(c, "echo still");
        a.eq(r2, "still\n", "the connection is still usable");
        c.close();
    }, { timeoutMs: 90000 });

    async function exec(c, cmd) { return text((await run(c, cmd)).stdout); }

    t.test("sshd with AllowTcpForwarding no refuses both", async (a) => {
        const s = keep(await startSshd({ tag: "cfwdno", extra: ["AllowTcpForwarding no"] }));
        const c = await SSHClient.connect(s.base);
        let err = "";
        try { await c.forwardRemote({ remotePort: 0, host: "127.0.0.1", port: echo.port }); } catch (e) { err = String(e); }
        a.ok(err !== "", "forwardRemote rejected: " + err);
        const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
        const r = await exchange(f.listenPort, bytes(1000, 3), { timeoutMs: 10000 });
        a.eq(r.bytes.byteLength, 0, "the local connection closes with no bytes");
        f.close();
        c.close();
        s.close();
    }, { timeoutMs: 60000 });

    t.test("via: through sshd to sshd, and through sshd to this module's server", async (a) => {
        const s = await sshd();
        const jump = await SSHClient.connect(s.base);
        const inner = await SSHClient.connect({ ...s.base, via: jump });
        a.eq(await exec(inner, "echo inner"), "inner\n");
        inner.close();
        const hk = keygen(s.dir + "/ours");
        const ours = keep(SSHServer.listen({ hostKey: hk.priv, authorizedKeys: s.user.pub + "\n", onSession: async (x) => { await x.write("ours " + x.command + "\n"); } }));
        write(s.dir + "/kh-ours", "[127.0.0.1]:" + ours.port + " " + pubBlob(hk.pub) + "\n");
        const c2 = await SSHClient.connect({ ...s.base, port: ours.port, knownHosts: s.dir + "/kh-ours", via: jump });
        a.eq(await exec(c2, "x"), "ours x\n");
        c2.close();
        let err = "";
        try { await SSHClient.connect({ ...s.base, port: ours.port, via: jump }); } catch (e) { err = String(e); }
        a.ok(/host key/i.test(err), "the target is checked against its own known_hosts, not the jump's: " + err);
        jump.close();
    }, { timeoutMs: 60000 });
}

await t.run();
