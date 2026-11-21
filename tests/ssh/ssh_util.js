// Shared harness for the dyna:ssh suites: a private OpenSSH sshd on 127.0.0.1 with a temp config, an ed25519 (or, via hostType, rsa) host key and
// ed25519 user keys generated at run time under TMPDIR (nothing secret is checked in), KEX and ciphers restricted to the
// allowed set; a shell watchdog kills sshd when this process dies, so a killed suite leaves no daemon behind, and close() kills sshd through its pid file. tamperProxy forwards to a port and flips one server-to-client byte at a given offset. PerSourcePenalties is off so refused handshakes from the negative suites do not penalise later connects.
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import { TCPServer } from "dyna:net";

export const SSHD = "/usr/sbin/sshd";
function opensshVersion(cmd, args) {
    try {
        const r = sys.Exec(cmd, args);
        const m = /OpenSSH_(\d+)\.(\d+)/.exec(r.stderr + r.stdout);
        return m ? [+m[1], +m[2]] : null;
    } catch { return null; }
}
const atLeast99 = (v) => v !== null && (v[0] > 9 || (v[0] === 9 && v[1] >= 9));
export const SSHD_VERSION = (() => { try { return os.stat(SSHD)[1] === 0 ? opensshVersion(SSHD, ["-V"]) : null; } catch { return null; } })();
export const SSH_VERSION = sys.Which("ssh") ? opensshVersion("ssh", ["-V"]) : null;
export const HAVE_KEYGEN = sys.Which("ssh-keygen") !== null;
export const HAVE_SSHD = HAVE_KEYGEN && atLeast99(SSHD_VERSION);
export const HAVE_SSH = HAVE_KEYGEN && atLeast99(SSH_VERSION);
export const SKIP_WHY = !HAVE_KEYGEN ? "no ssh-keygen on this host" : SSHD_VERSION === null ? "no /usr/sbin/sshd on this host" : "sshd " + SSHD_VERSION.join(".") + " is older than 9.9 (no mlkem768x25519-sha256)";
export const SKIP_WHY_SSH = !HAVE_KEYGEN ? "no ssh-keygen on this host" : SSH_VERSION === null ? "no ssh client on this host" : "ssh " + SSH_VERSION.join(".") + " is older than 9.9 (no mlkem768x25519-sha256)";

export async function collect(pipe) {
    const parts = [];
    let n = 0;
    for await (const c of pipe) { parts.push(c); n += c.byteLength; }
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.byteLength; }
    return out;
}

export async function sshCli(args, opts = {}) {
    const p = new sys.Spawn("ssh", args, { input: opts.input, timeoutMs: opts.timeoutMs || 30000 });
    try {
        const [out, err] = await Promise.all([collect(p.stdout), collect(p.stderr)]);
        const r = await p.wait();
        return { code: r.code, signal: r.signal, stdout: out, stderr: new TextDecoder().decode(err) };
    } finally { p.close(); }
}

export function certify(dir, ca, pubPath, opts) {
    const args = ["-q", "-s", ca, "-I", opts.id || "dyna-cert", "-n", opts.principals, "-V", opts.validity || "-5m:+1h"];
    if (opts.host) args.push("-h");
    for (const o of opts.options || []) args.push("-O", o);
    args.push(pubPath);
    const r = sys.Exec("ssh-keygen", args);
    if (r.code !== 0) throw new Error("ssh-keygen -s failed: " + r.stderr);
    return read(pubPath.replace(/\.pub$/, "") + "-cert.pub").trim();
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function write(path, text) {
    const f = std.open(path, "w");
    if (!f) throw new Error("cannot write " + path);
    f.puts(text);
    f.close();
}

export function read(path) {
    return std.loadFile(path);
}

export function tmpDir(tag) {
    const base = std.getenv("TMPDIR") || "/tmp";
    const d = base.replace(/\/$/, "") + "/ssh-" + tag + "-" + sys.pid() + "-" + Math.floor(Math.random() * 1e9);
    os.mkdir(d, 0o700);
    return d;
}

export function keygen(path, opts = {}) {
    const args = ["-q", "-t", opts.type || "ed25519", "-N", opts.pass || "", "-f", path, "-C", "dyna-test"];
    if (opts.cipher) args.push("-Z", opts.cipher);
    if (opts.rounds) args.push("-a", String(opts.rounds));
    const r = sys.Exec("ssh-keygen", args);
    if (r.code !== 0) throw new Error("ssh-keygen failed: " + r.stderr);
    return { priv: read(path), pub: read(path + ".pub").trim() };
}

export function pubBlob(pubLine) {
    const p = pubLine.split(/\s+/);
    return p[0] + " " + p[1];
}

export async function startSshd(opts = {}) {
    const dir = opts.dir || tmpDir(opts.tag || "sshd");
    const host = keygen(dir + "/host", opts.hostType ? { type: opts.hostType } : {});
    const user = keygen(dir + "/user");
    const prepared = opts.prepare ? opts.prepare(dir) : [];
    const keys = [user.pub].concat(opts.extraAuth || []);
    write(dir + "/auth", keys.join("\n") + "\n");
    const kex = opts.kex || "mlkem768x25519-sha256,sntrup761x25519-sha512,curve25519-sha256";
    const ciphers = opts.ciphers || "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com";
    let lastErr = "";
    for (let attempt = 0; attempt < 8; attempt++) {
        const port = 20000 + Math.floor(Math.random() * 30000);
        const lines = [
            "Port " + port, "ListenAddress 127.0.0.1", "HostKey " + dir + "/host", "PidFile " + dir + "/pid",
            "AuthorizedKeysFile " + dir + "/auth", "StrictModes no", "PasswordAuthentication no",
            "KbdInteractiveAuthentication no", "PubkeyAuthentication yes", "UsePAM no", "PerSourcePenalties no",
            "MaxStartups 1024", "MaxSessions 1024", "KexAlgorithms " + kex, "Ciphers " + ciphers,
            "HostKeyAlgorithms " + (opts.hostAlgs || "ssh-ed25519"), "PubkeyAcceptedAlgorithms " + (opts.userAlgs || "ssh-ed25519"), "LogLevel " + (opts.logLevel || "DEBUG1"),
        ].concat(opts.extra || [], prepared);
        write(dir + "/sshd_config", lines.join("\n") + "\n");
        const log = dir + "/sshd." + attempt + ".log";
        const watch = SSHD + " -D -e -f " + dir + "/sshd_config -E " + log + " & S=$!; while kill -0 " + sys.pid() + " 2>/dev/null && kill -0 $S 2>/dev/null; do sleep 0.5; done; kill $S 2>/dev/null";
        const proc = new sys.Spawn("/bin/sh", ["-c", watch]);
        for (let i = 0; i < 200; i++) {
            await sleep(20);
            const t = read(log) || "";
            if (/Server listening on/.test(t)) {
                const known = dir + "/known_hosts";
                write(known, "[127.0.0.1]:" + port + " " + pubBlob(host.pub) + "\n");
                return {
                    port, dir, log, proc, host, user, known,
                    base: { host: "127.0.0.1", port, username: sys.getEnv("USER") || "root", privateKey: user.priv, knownHosts: known },
                    logText: () => read(log) || "",
                    close: () => { try { const pid = Number((read(dir + "/pid") || "").trim()); if (pid > 1) os.kill(pid, os.SIGTERM); } catch {} try { proc.kill("SIGTERM"); } catch {} try { proc.close(); } catch {} },
                };
            }
            if (/Address already in use|fatal/.test(t)) { lastErr = t; break; }
        }
        try { proc.close(); } catch {}
    }
    throw new Error("sshd did not start: " + lastErr);
}

export async function readAll(rs) {
    const r = rs.getReader();
    const parts = [];
    let n = 0;
    for (;;) {
        const { value, done } = await r.read();
        if (done) break;
        parts.push(value);
        n += value.byteLength;
    }
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.byteLength; }
    return out;
}

export const text = (u8) => new TextDecoder().decode(u8);

export async function run(client, cmd, input) {
    const x = await client.exec(cmd);
    if (input !== undefined) { await x.write(input); x.end(); }
    const [out, err] = await Promise.all([readAll(x.stdout), readAll(x.stderr)]);
    const ex = await x.exit;
    return { stdout: out, stderr: err, exit: ex };
}

export function tamperProxy(port, flipAt) {
    const srv = new TCPServer({ port: 0 });
    const pairs = new Map();
    srv.start({
        connect: (c) => {
            if (!c) return;
            const p = { down: 0, up: null, pend: [], c };
            pairs.set(c, p);
            p.client = TCPServer.connect({ host: "127.0.0.1", port }, {
                connect: (u) => { if (!u) { c.close(); return; } p.up = u; for (const b of p.pend) u.write(b); p.pend = []; },
                data: (u, b) => {
                    if (p.down <= flipAt && flipAt < p.down + b.length) { b = b.slice(); b[flipAt - p.down] ^= 0x40; p.flipped = true; }
                    p.down += b.length;
                    c.write(b);
                },
                close: () => { try { c.close(); } catch {} },
            });
        },
        data: (c, b) => { const p = pairs.get(c); if (p.up) p.up.write(b); else p.pend.push(b); },
        close: (c) => { const p = pairs.get(c); if (p) { try { p.client.close(); } catch {} } },
    });
    return { port: srv.port, flipped: () => [...pairs.values()].some((p) => p.flipped), close: () => { for (const p of pairs.values()) { try { p.client.close(); } catch {} } srv.close(); } };
}
