// flags: --std
// covers: dyna:ssh.SSHServer -- hostile forwarding messages from a scripted client (fake_client.js) after authentication
// timeout: 120
// A direct-tcpip CHANNEL_OPEN whose payload is truncated gets OPEN_FAILURE or ends the connection, never a hook call. CHANNEL_DATA
// and CHANNEL_CLOSE on a channel still pending the forward hook's Promise end the connection (protocol error) and the late
// decision is dropped. An unsolicited REQUEST_SUCCESS / REQUEST_FAILURE to the server is ignored (the connection still answers a
// global request). More than 64 tcpip-forward requests parked on the hook: the excess ones are refused or the connection ends,
// and the server keeps accepting. A tcpip-forward with a truncated payload is refused. After every case the server still
// authenticates a new client and its stats show no stray channels.
import { SSHServer } from "dyna:ssh";
import { Ed25519Generate } from "dyna:crypto";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir, keygen, sleep } from "./ssh_util.js";
import { FakeClient, authLine } from "./fake_client.js";
import { W } from "./fake_server.js";

const t = suite("ssh_server_forward_malformed");
const servers = [];
t.after(() => { for (const s of servers) { try { s.close(); } catch {} } });

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const dir = tmpDir("fwdmal");
    const host = keygen(dir + "/host");
    const good = Ed25519Generate();
    let hookCalls = 0;
    const parked = [];
    const listen = (forward) => {
        const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: authLine(good.publicKey) + "\n", onSession: (x) => x.exit(0), forward });
        servers.push(s);
        return s;
    };
    const login = async (s) => {
        const c = new FakeClient(s.port);
        await c.handshake();
        const r = await c.auth("eve", { publicKey: good.publicKey, signWith: good.privateKey });
        if (r !== 52) throw new Error("auth failed: " + r);
        return c;
    };
    const within = async (c, ms, ...types) => {
        const x = await Promise.race([c.expect(...types), sleep(ms).then(() => "timeout")]);
        return x === null ? "closed" : x === "timeout" ? "timeout" : x[0];
    };
    const directOpen = (id, payloadTail) => new W().u8(90).str("direct-tcpip").u32(id).u32(1 << 21).u32(32768).raw(payloadTail).done();
    const healthy = async (a, s, what) => {
        const c = await login(s);
        c.send(new W().u8(80).str("keepalive@example").bool(true).done());
        a.eq(await within(c, 3000, 81, 82), 82, what + ": a new connection authenticates and answers a global request");
        c.close();
        await sleep(50);
        a.eq(s.stats.channels, 0, what + ": no stray channels");
    };

    t.test("a truncated direct-tcpip open is refused before the hook", async (a) => {
        hookCalls = 0;
        const s = listen(() => { hookCalls++; return true; });
        const c = await login(s);
        c.send(directOpen(1, new W().str("127.0.0.1").u32(9).str("127.0.0.1").done()));
        const r = await within(c, 3000, 92, 91, 1);
        a.ok(r === 92 || r === 1 || r === "closed", "OPEN_FAILURE or the connection ends: " + r);
        a.eq(hookCalls, 0, "the hook never saw a malformed request");
        c.close();
        await healthy(a, s, "truncated open");
    });

    t.test("DATA or CLOSE on a channel pending the hook ends the connection", async (a) => {
        for (const msg of [(id) => new W().u8(94).u32(id).str("early").done(), (id) => new W().u8(97).u32(id).done()]) {
            const s = listen(() => new Promise((res) => parked.push(res)));
            const c = await login(s);
            c.send(directOpen(7, new W().str("127.0.0.1").u32(9).str("127.0.0.1").u32(5555).done()));
            await sleep(100);
            c.send(msg(0));
            const r = await within(c, 3000, 1, 91, 92);
            a.ok(r === 1 || r === "closed", "the connection ends: " + r);
            c.close();
            for (const p of parked.splice(0)) p(true);
            await sleep(50);
            await healthy(a, s, "pending-channel message");
        }
    });

    t.test("unsolicited REQUEST_SUCCESS / FAILURE to the server is ignored", async (a) => {
        const s = listen(() => true);
        const c = await login(s);
        c.send(new W().u8(81).u32(1234).done());
        c.send(new W().u8(82).done());
        c.send(new W().u8(80).str("keepalive@example").bool(true).done());
        a.eq(await within(c, 3000, 81, 82, 1), 82, "the connection still answers a global request");
        c.close();
        await healthy(a, s, "unsolicited reply");
    });

    t.test("more than 64 parked tcpip-forward requests and a truncated one", async (a) => {
        const s = listen(() => new Promise((res) => parked.push(res)));
        const c = await login(s);
        for (let i = 0; i < 70; i++) c.send(new W().u8(80).str("tcpip-forward").bool(true).str("127.0.0.1").u32(0).done());
        await sleep(200);
        a.ok(parked.length <= 64, "at most 64 requests reach the hook: " + parked.length);
        for (const p of parked.splice(0)) p(false);
        let fails = 0, other = "";
        for (let i = 0; i < 70; i++) { const r = await within(c, 2000, 81, 82, 1); if (r === 82) fails++; else { other = String(r); break; } }
        a.ok(fails === 70 || other === "1" || other === "closed", "every request refused in order, or the connection ended: " + fails + " " + other);
        c.close();
        await healthy(a, s, "64 parked");
        const s2 = listen(() => true);
        const c2 = await login(s2);
        c2.send(new W().u8(80).str("tcpip-forward").bool(true).str("127.0.0.1").done());
        const r2 = await within(c2, 3000, 81, 82, 1);
        a.ok(r2 === 82 || r2 === 1 || r2 === "closed", "a truncated tcpip-forward is refused: " + r2);
        c2.close();
        await healthy(a, s2, "truncated tcpip-forward");
    });
}

await t.run();
