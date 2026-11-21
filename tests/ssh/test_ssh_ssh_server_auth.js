// flags: --std
// covers: dyna:ssh.SSHServer -- publickey signature verification against a scripted JS client: forged, corrupted and sk-ssh-ed25519 signatures with and without user presence
// timeout: 60
// fake_client.js speaks curve25519-sha256 + aes256-gcm@openssh.com with strict KEX and sends USERAUTH_REQUESTs no real client sends.
// A listed key with a valid signature succeeds (52); the same key signed by another private key, or with one signature bit flipped,
// fails (51) even though the key is in authorizedKeys; an sk-ssh-ed25519@openssh.com key succeeds only when the user-presence flag
// is set; the authorize hook is never called for a request whose signature fails. The suite SKIPs without ssh-keygen.
import { SSHServer } from "dyna:ssh";
import { Ed25519Generate } from "dyna:crypto";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir, keygen } from "./ssh_util.js";
import { FakeClient, authLine } from "./fake_client.js";

const t = suite("ssh_server_auth");
const servers = [];
t.after(() => { for (const s of servers) { try { s.close(); } catch {} } });

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const dir = tmpDir("srvauth");
    const host = keygen(dir + "/host");
    const good = Ed25519Generate();
    const evil = Ed25519Generate();
    const sk = Ed25519Generate();
    let hookCalls = 0;
    const listen = (extra = {}) => {
        const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: authLine(good.publicKey) + "\n" + authLine(sk.publicKey, true) + "\n", onSession: (x) => x.exit(0), ...extra });
        servers.push(s);
        return s;
    };
    const attempt = async (s, o) => {
        const c = new FakeClient(s.port);
        try { await c.handshake(); return await c.auth("dave", o); } finally { c.close(); }
    };

    t.test("valid signature succeeds", async (a) => {
        const s = listen();
        a.eq(await attempt(s, { publicKey: good.publicKey, signWith: good.privateKey }), 52);
    });

    t.test("signature by another key fails although the key is listed", async (a) => {
        const s = listen();
        a.eq(await attempt(s, { publicKey: good.publicKey, signWith: evil.privateKey }), 51);
    });

    t.test("corrupted signature fails", async (a) => {
        const s = listen();
        a.eq(await attempt(s, { publicKey: good.publicKey, signWith: good.privateKey, corrupt: true }), 51);
    });

    t.test("sk-ssh-ed25519: user presence required", async (a) => {
        const s = listen();
        a.eq(await attempt(s, { sk: true, publicKey: sk.publicKey, signWith: sk.privateKey, flags: 1 }), 52);
        a.eq(await attempt(s, { sk: true, publicKey: sk.publicKey, signWith: sk.privateKey, flags: 0 }), 51);
        a.eq(await attempt(s, { sk: true, publicKey: sk.publicKey, signWith: evil.privateKey, flags: 1 }), 51);
    });

    t.test("the authorize hook only sees verified requests", async (a) => {
        hookCalls = 0;
        const s = listen({ authorize: () => { hookCalls++; return true; } });
        a.eq(await attempt(s, { publicKey: evil.publicKey, signWith: good.privateKey }), 51);
        a.eq(hookCalls, 0);
        a.eq(await attempt(s, { publicKey: evil.publicKey, signWith: evil.privateKey }), 52);
        a.eq(hookCalls, 1);
    });
}

await t.run();
