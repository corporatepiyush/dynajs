// flags: --std
// covers: dyna:ssh.SSHClient -- host key verification against known_hosts: strict refusal of unknown and changed keys, the accept-new opt-in (which writes hashed entries that ssh-keygen -F finds), hashed entries, patterns, @revoked
// timeout: 120
// Against a private sshd: the host key must match a known_hosts entry for "[127.0.0.1]:PORT". Unknown hosts are refused unless
// dyna.hostKeyPolicy is "accept-new", which appends the key and then behaves strictly; a CHANGED key is refused under both policies
// (the message says HOST KEY MISMATCH); hashed (|1|salt|hash, ssh-keygen -H) entries, wildcard and negated patterns match; a key
// marked @revoked is refused even when another line trusts it; a missing file means unknown.
import { SSHClient } from "dyna:ssh";
import * as sys from "dyna:sys";
import { suite } from "../lib/kit.js";
import { HAVE_SSHD, SKIP_WHY, startSshd, keygen, read, write, pubBlob, run, text } from "./ssh_util.js";

const t = suite("ssh_known_hosts");
let d;
t.after(() => { if (d) d.close(); });

if (!HAVE_SSHD) t.skip("sshd", SKIP_WHY);
else {
    t.before(async () => { d = await startSshd({ tag: "kh" }); });
    const kh = (name, body) => { const p = d.dir + "/" + name; if (body !== undefined) write(p, body); return p; };
    const hostLine = () => pubBlob(d.host.pub);
    const hp = () => "[127.0.0.1]:" + d.port;

    t.test("known key accepted", async (a) => {
        const c = await SSHClient.connect(d.base);
        a.eq(text((await run(c, "echo ok")).stdout), "ok\n");
        c.close();
    });

    t.test("unknown host refused by default", async (a) => {
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: kh("empty", "# nothing\n") }), /host key for 127\.0\.0\.1 is not in .*SHA256:/);
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: d.dir + "/missing-file" }), /is not in/);
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: kh("otherport", "[127.0.0.1]:1 " + hostLine() + "\n") }), /is not in/);
    });

    t.test("changed key refused under both policies", async (a) => {
        const other = keygen(d.dir + "/other");
        const p = kh("changed", hp() + " " + pubBlob(other.pub) + "\n");
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: p }), /HOST KEY MISMATCH/);
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: p, dyna: { hostKeyPolicy: "accept-new" } }), /HOST KEY MISMATCH/);
        a.eq(read(p), hp() + " " + pubBlob(other.pub) + "\n", "accept-new did not touch a CHANGED entry");
    });

    t.test("accept-new appends then trusts", async (a) => {
        const p = d.dir + "/fresh_known_hosts";
        const c = await SSHClient.connect({ ...d.base, knownHosts: p, dyna: { hostKeyPolicy: "accept-new" } });
        c.close();
        a.match(read(p), /^\|1\|[A-Za-z0-9+\/]{27}=\|[A-Za-z0-9+\/]{27}= ssh-ed25519 \S+\n$/, "accept-new writes one hashed entry");
        a.eq(read(p).split(" ").slice(1).join(" "), hostLine() + "\n");
        a.ok(!read(p).includes("127.0.0.1"), "the host name is not stored in clear");
        const f = sys.Exec("ssh-keygen", ["-F", hp(), "-f", p]);
        a.eq(f.code, 0, "ssh-keygen -F finds the hashed entry (same HMAC-SHA1 as OpenSSH)");
        const g = sys.Exec("ssh-keygen", ["-F", "[127.0.0.1]:" + (d.port + 1), "-f", p]);
        a.ne(g.code, 0, "no match for another port");
        const c2 = await SSHClient.connect({ ...d.base, knownHosts: p });
        a.eq(c2.hostKey.type, "ssh-ed25519");
        c2.close();
        a.eq(read(p).split("\n").length, 2, "no second line on the strict reconnect");
    });

    t.test("hashed entry", async (a) => {
        const p = kh("hashed", hp() + " " + hostLine() + "\n");
        const r = sys.Exec("ssh-keygen", ["-H", "-f", p]);
        a.eq(r.code, 0);
        a.match(read(p), /^\|1\|[^|]+\|\S+ ssh-ed25519 /);
        const c = await SSHClient.connect({ ...d.base, knownHosts: p });
        c.close();
        const other = keygen(d.dir + "/other2");
        const q = kh("hashed2", hp() + " " + pubBlob(other.pub) + "\n");
        sys.Exec("ssh-keygen", ["-H", "-f", q]);
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: q }), /HOST KEY MISMATCH/);
    });

    t.test("patterns and negation", async (a) => {
        const c = await SSHClient.connect({ ...d.base, knownHosts: kh("wild", "foo,[127.0.0.*]:" + d.port + " " + hostLine() + "\n") });
        c.close();
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: kh("neg", "[127.0.0.*]:" + d.port + ",!" + hp() + " " + hostLine() + "\n") }), /is not in/);
        const c2 = await SSHClient.connect({ ...d.base, knownHosts: kh("crlf", "# c\r\n\r\n" + hp() + " " + hostLine() + " comment\r\n") });
        c2.close();
    });

    t.test("@revoked wins", async (a) => {
        const p = kh("revoked", hp() + " " + hostLine() + "\n@revoked * " + hostLine() + "\n");
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: p }), /@revoked/);
    });
}

await t.run();
