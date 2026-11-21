// flags: --std
// covers: dyna:ssh.SSHClient -- OpenSSH certificates against the system sshd (>= 9.9): a user certificate (TrustedUserCAKeys), a host certificate (HostCertificate) through @cert-authority
// timeout: 120
// A private sshd trusts a run-time CA for user certificates and presents a host certificate signed by a second CA. The client logs in
// with options.certificate (the bare key is not in authorized_keys, so only the certificate can succeed), a certificate for another
// principal or from an untrusted CA is refused by sshd, and the host certificate verifies only through a matching @cert-authority line
// (a wrong CA, or the CA marked @revoked, is refused; a plain known_hosts line still works and negotiates ssh-ed25519). The suite SKIPs
// when /usr/sbin/sshd or ssh-keygen is missing or sshd is older than 9.9.
import { SSHClient } from "dyna:ssh";
import * as sys from "dyna:sys";
import { suite } from "../lib/kit.js";
import { HAVE_SSHD, SKIP_WHY, startSshd, keygen, write, pubBlob, run, text, tmpDir, certify } from "./ssh_util.js";

const t = suite("ssh_client_certs");
let d;
t.after(() => { if (d) d.close(); });

if (!HAVE_SSHD) t.skip("sshd", SKIP_WHY);
else {
    const dir = tmpDir("ccert");
    const uca = keygen(dir + "/uca");
    const hca = keygen(dir + "/hca");
    const other = keygen(dir + "/other");
    const me = keygen(dir + "/me");
    const login = sys.getEnv("USER") || "root";

    t.before(async () => {
        const sdir = tmpDir("ccsshd");
        write(dir + "/uca.pub.list", uca.pub + "\n");
        d = await startSshd({
            tag: "certs", dir: sdir, extraAuth: [],
            hostAlgs: "ssh-ed25519-cert-v01@openssh.com,ssh-ed25519", userAlgs: "ssh-ed25519,ssh-ed25519-cert-v01@openssh.com",
            prepare: (sd) => { certify(sd, dir + "/hca", sd + "/host.pub", { principals: "127.0.0.1", host: true, id: "sshd-host" }); return ["TrustedUserCAKeys " + dir + "/uca.pub.list", "HostCertificate " + sd + "/host-cert.pub"]; },
        });
    });
    const certFor = (name, opts) => { sys.Exec("cp", [dir + "/me.pub", dir + "/" + name + ".pub"]); return certify(dir, dir + "/" + (opts.ca || "uca"), dir + "/" + name + ".pub", opts); };

    t.test("user certificate login", async (a) => {
        const cert = certFor("c_ok", { principals: login, id: "me-cert" });
        const c = await SSHClient.connect({ ...d.base, privateKey: me.priv, certificate: cert });
        a.eq(text((await run(c, "echo cert")).stdout), "cert\n");
        c.close();
        await a.rejects(SSHClient.connect({ ...d.base, privateKey: me.priv }), /was refused/, "the bare key alone is not authorized");
        await a.rejects(SSHClient.connect({ ...d.base, privateKey: me.priv, certificate: certFor("c_pr", { principals: "nobody-else" }) }), /ssh-ed25519-cert-v01@openssh.com key was refused/);
        await a.rejects(SSHClient.connect({ ...d.base, privateKey: me.priv, certificate: certFor("c_ca", { principals: login, ca: "hca" }) }), /was refused/);
        a.throws(() => SSHClient.connect({ ...d.base, privateKey: other.priv, certificate: cert }), /not a user certificate for privateKey/);
    });

    t.test("host certificate through @cert-authority", async (a) => {
        const p = dir + "/kh_ca";
        write(p, "@cert-authority [127.0.0.1]:" + d.port + " " + pubBlob(hca.pub) + "\n");
        const c = await SSHClient.connect({ ...d.base, knownHosts: p });
        a.eq(c.algorithms.hostKey, "ssh-ed25519-cert-v01@openssh.com");
        a.eq(c.hostKey.type, "ssh-ed25519-cert-v01@openssh.com");
        a.eq(text((await run(c, "echo hc")).stdout), "hc\n");
        c.close();
        write(dir + "/kh_wrong", "@cert-authority * " + pubBlob(uca.pub) + "\n");
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: dir + "/kh_wrong" }), /no @cert-authority line/);
        write(dir + "/kh_rev", "@cert-authority * " + pubBlob(hca.pub) + "\n@revoked * " + pubBlob(hca.pub) + "\n");
        await a.rejects(SSHClient.connect({ ...d.base, knownHosts: dir + "/kh_rev" }), /@revoked/);
        const plain = await SSHClient.connect(d.base);
        a.eq(plain.algorithms.hostKey, "ssh-ed25519");
        plain.close();
    });
}

await t.run();
