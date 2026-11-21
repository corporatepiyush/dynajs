// flags: --std
// covers: dyna:ssh.SSHServer -- interop: the OpenSSH ssh client's -L, -R and -J against this module's server
// timeout: 180
// ssh -N -L: 4 MiB byte-exact through direct-tcpip and a half-close answered after EOF; the forward hook saw type "local". ssh -N
// -R on a fixed port: 2 MiB byte-exact through forwarded-tcpip (the hook saw type "remote"); a server without a hook makes
// ExitOnForwardFailure end ssh with an error. ssh -J: OpenSSH's ProxyJump through one of our servers (forward hook on) to another
// runs a command. Every ssh child is stopped by the suite. The suite SKIPs without an OpenSSH >= 9.9 ssh client.
import { SSHServer } from "dyna:ssh";
import * as sys from "dyna:sys";
import { TCPServer } from "dyna:net";
import { suite } from "../lib/kit.js";
import { HAVE_SSH, SKIP_WHY_SSH, tmpDir, keygen, write, pubBlob, sleep, sshCli, text } from "./ssh_util.js";
import { echoTarget, halfTarget, exchange } from "./fwd_util.js";

const t = suite("ssh_server_forward_openssh");
const closers = [];
t.after(() => { for (const s of closers) { try { s.close(); } catch {} } });

function bytes(n, seed) { const u = new Uint8Array(n); let x = seed >>> 0 || 1; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; u[i] = x >>> 24; } return u; }
function same(a, b) { if (a.byteLength !== b.byteLength) return false; for (let i = 0; i < a.byteLength; i++) if (a[i] !== b[i]) return false; return true; }
function freePort() { const s = new TCPServer({ port: 0 }); s.start({}); const p = s.port; s.close(); return p; }

if (!HAVE_SSH) t.skip("ssh", SKIP_WHY_SSH);
else {
    const dir = tmpDir("fwdossh");
    const host = keygen(dir + "/host");
    const host2 = keygen(dir + "/host2");
    const user = keygen(dir + "/user");
    const keep = (x) => { closers.push(x); return x; };
    const echo = keep(echoTarget());
    const half = keep(halfTarget());
    const seen = [];
    const listen = (opts = {}, hk = host) => keep(SSHServer.listen({
        hostKey: hk.priv, authorizedKeys: user.pub + "\n",
        onSession: async (s) => { await s.write("hello " + (s.command || "") + "\n"); }, ...opts,
    }));
    const kh = dir + "/kh";
    const cfgFor = (servers) => {
        write(kh, servers.map(([s, hk]) => "[127.0.0.1]:" + s.port + " " + pubBlob(hk.pub)).join("\n") + "\n");
        write(dir + "/cfg", ["Host *", "  IdentityFile " + dir + "/user", "  IdentitiesOnly yes", "  BatchMode yes", "  UserKnownHostsFile " + kh,
            "  StrictHostKeyChecking yes", "  LogLevel ERROR", "  ExitOnForwardFailure yes", "  ServerAliveInterval 0", ""].join("\n"));
        return dir + "/cfg";
    };
    const bg = (args) => { const p = new sys.Spawn("ssh", args); keep({ close: () => { try { p.kill("SIGTERM"); } catch {} try { p.close(); } catch {} } }); return p; };

    t.test("ssh -L through our server", async (a) => {
        const s = listen({ forward: (r) => { seen.push(r); return true; } });
        const cfg = cfgFor([[s, host]]);
        const lp = freePort(), lp2 = freePort();
        const p = bg(["-F", cfg, "-N", "-p", String(s.port), "-L", "127.0.0.1:" + lp + ":127.0.0.1:" + echo.port, "-L", "127.0.0.1:" + lp2 + ":127.0.0.1:" + half.port, "alice@127.0.0.1"]);
        let r = null;
        const data = bytes(4 << 20, 6);
        for (let i = 0; i < 100; i++) { r = await exchange(lp, data, { timeoutMs: 20000 }); if (r.bytes.byteLength) break; await sleep(50); }
        a.ok(same(r.bytes, data), "4 MiB byte-exact through ssh -L: " + r.bytes.byteLength);
        a.eq(text((await exchange(lp2, bytes(70000, 2))).bytes), "got 70000", "half-close through ssh -L");
        a.eq(seen[seen.length - 1].type, "local");
        p.kill("SIGTERM");
        p.close();
    }, { timeoutMs: 90000 });

    t.test("ssh -R through our server; refused without a hook", async (a) => {
        let hooked;
        const ready = new Promise((res) => { hooked = res; });
        const s = listen({ forward: (r) => { seen.push(r); setTimeout(hooked, 100); return true; } });
        const cfg = cfgFor([[s, host]]);
        const rp = freePort();
        const p = bg(["-F", cfg, "-N", "-p", String(s.port), "-R", "127.0.0.1:" + rp + ":127.0.0.1:" + echo.port, "alice@127.0.0.1"]);
        await ready;
        const req = seen[seen.length - 1];
        a.eq(req.type, "remote");
        a.eq(req.port, rp);
        const data = bytes(2 << 20, 8);
        const r = await exchange(rp, data);
        a.ok(same(r.bytes, data), "2 MiB byte-exact through ssh -R: " + r.bytes.byteLength);
        p.kill("SIGTERM");
        p.close();
        const s2 = listen();
        const cfg2 = cfgFor([[s2, host]]);
        const no = await sshCli(["-F", cfg2, "-N", "-p", String(s2.port), "-R", "127.0.0.1:" + freePort() + ":127.0.0.1:" + echo.port, "alice@127.0.0.1"], { timeoutMs: 20000 });
        a.ok(no.code !== 0, "ExitOnForwardFailure: exit " + no.code + " " + no.stderr.trim());
    }, { timeoutMs: 90000 });

    t.test("ssh -J through our server to our server", async (a) => {
        const jump = listen({ forward: () => true });
        const target = listen({}, host2);
        const cfg = cfgFor([[jump, host], [target, host2]]);
        const r = await sshCli(["-F", cfg, "-J", "alice@127.0.0.1:" + jump.port, "-p", String(target.port), "alice@127.0.0.1", "via-jump"], { timeoutMs: 30000 });
        a.eq(r.code, 0, r.stderr);
        a.eq(text(r.stdout), "hello via-jump\n");
    }, { timeoutMs: 60000 });
}

await t.run();
