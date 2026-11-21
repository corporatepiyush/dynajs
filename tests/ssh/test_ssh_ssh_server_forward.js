// flags: --std
// covers: integration dyna:ssh.SSHClient.forwardLocal dyna:ssh.SSHClient.forwardRemote dyna:ssh.SSHServer -- port forwarding and ProxyJump between this module's client and server on 127.0.0.1
// timeout: 180
// Local forwards (direct-tcpip): 8 MiB byte-exact both ways through the C relay, half-close in both directions (the target answers
// "got N" after the client's FIN), the server's forward hook (what it sees; sync false, throw, non-boolean and Promise results;
// no hook means refused), an unreachable target (OPEN_FAILURE, the local socket closes). Remote forwards (tcpip-forward): port 0
// allocation echoed back, 2 MiB byte-exact, two remote forwards each reach their own target, close() sends cancel-tcpip-forward and the server stops listening, a server without a
// hook refuses, a requested "0.0.0.0" binds 127.0.0.1 unless dyna.publicForwardBinds (the bound address read with lsof, else a
// connect to a non-loopback address of this host), at most 16 remote forwards per connection. A user certificate without permit-port-forwarding gets
// both refused. ProxyJump (connect({via})): exec through the jump, the target's host key is checked against the target's own
// known_hosts entry (a mismatch fails although the jump is trusted), closing the inner client leaves the jump usable, closing the
// jump ends the inner client. Ending the SSH connection closes every listener; five rounds of forwards after a warm-up
// round leave the fd count no higher. With a target that never reads, 48 MiB pushed into a local forward leave the client's
// buffers under 6 MiB (the socket is read only while the channel queue is below 256 KiB). A slow local reader still gets every byte when the peer sends EOF and CLOSE while the relay
// has bytes queued to the socket (the relay drains them before it closes the socket); a socket that never reads holds those
// bytes only until the SSH connection ends (the fd is closed then), and at most dyna.maxChannels such sockets drain per connection
// (a third, with maxChannels 2, is closed at once). A via chain nests two hops; closing the first jump ends both.
// Process teardown (ssh_fwd_teardown_child.js) with live local/remote forwards and a via chain: the exit code is the script's and the
// runtime frees cleanly; 80 forwarded connections (local, via, remote) then a clean exit report no leak under LeakSanitizer (the
// sanitizer build: ASAN_OPTIONS=detect_leaks=1 is set for that child). Option validation. The suite SKIPs without ssh-keygen.
import { SSHClient, SSHServer } from "dyna:ssh";
import * as sys from "dyna:sys";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import { HAVE_KEYGEN, tmpDir, keygen, write, pubBlob, run, sleep, certify } from "./ssh_util.js";
import { echoTarget, halfTarget, burstTarget, stallTarget, acceptedSockets, exchange, fdCount, lanAddress, boundAddress, text } from "./fwd_util.js";

const t = suite("ssh_server_forward");
const closers = [];
t.after(() => { for (const s of closers) { try { s.close(); } catch {} } });

function bytes(n, seed) { const u = new Uint8Array(n); let x = seed >>> 0 || 1; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; u[i] = x >>> 24; } return u; }
function same(a, b) { if (a.byteLength !== b.byteLength) return false; for (let i = 0; i < a.byteLength; i++) if (a[i] !== b[i]) return false; return true; }

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const dir = tmpDir("fwd");
    const host = keygen(dir + "/host");
    const host2 = keygen(dir + "/host2");
    const user = keygen(dir + "/user");
    keygen(dir + "/ca");
    const keep = (x) => { closers.push(x); return x; };
    const echo = keep(echoTarget());
    const half = keep(halfTarget());
    const seen = [];
    function listen(opts = {}, hk = host) {
        return keep(SSHServer.listen({
            hostKey: hk.priv, authorizedKeys: user.pub + "\n",
            onSession: async (s) => { await s.write("hello " + (s.command || "") + "\n"); },
            ...opts,
        }));
    }
    const kh = (s, hk = host) => { const p = dir + "/kh" + s.port + Math.floor(Math.random() * 1e6); write(p, "[127.0.0.1]:" + s.port + " " + pubBlob(hk.pub) + "\n"); return p; };
    const base = (s, more = {}) => ({ host: "127.0.0.1", port: s.port, username: "alice", privateKey: user.priv, knownHosts: kh(s), ...more });
    const allow = (req) => { seen.push(req); return true; };

    t.test("process teardown with live forwards, remote listeners and a via chain is clean", async (a) => {
        const bin = std.getenv("BIN") || "./dynajs";
        for (const mode of ["local-live", "remote-live", "via-live"]) {
            for (const [env, rc] of [["", 1], ["FWD_TEARDOWN_EXIT=1 ", 3]]) {
                const r = sys.Exec("/bin/sh", ["-c", env + bin + " --std tests/ssh/ssh_fwd_teardown_child.js " + mode + " </dev/null 2>&1"], { timeoutMs: 30000 });
                a.eq(r.code, rc, mode + " " + env + "exit code: " + r.stdout.slice(-300));
                a.ok(/forwards-live/.test(r.stdout), mode + " reached the live state");
                a.ok(!/Assertion failed|AddressSanitizer|LeakSanitizer|runtime error/.test(r.stdout), mode + " teardown clean");
            }
        }
    }, { timeoutMs: 120000 });

    t.test("forward churn frees every channel's state (LeakSanitizer when BIN is the sanitizer build)", async (a) => {
        const bin = std.getenv("BIN") || "./dynajs";
        const r = sys.Exec("/bin/sh", ["-c", "ASAN_OPTIONS=detect_leaks=1 " + bin + " --std tests/ssh/ssh_fwd_teardown_child.js churn-clean </dev/null 2>&1"], { timeoutMs: 60000 });
        a.eq(r.code, 0, "churn-clean exit code: " + r.stdout.slice(-600));
        a.ok(/forwards-live/.test(r.stdout), "40 local forwards (half through a via hop) and 40 remote-forward connections ran");
        a.ok(!/LeakSanitizer|AddressSanitizer|runtime error/.test(r.stdout), "no leak or sanitizer report: " + r.stdout.slice(0, 600));
    }, { timeoutMs: 90000 });

    t.test("a target that never reads bounds the relay's memory on both ends", async (a) => {
        const stall = keep(stallTarget());
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s));
        const f = c.forwardLocal({ host: "127.0.0.1", port: stall.port });
        const pending = exchange(f.listenPort, new Uint8Array(48 << 20), { keepOpen: true, timeoutMs: 4000 });
        let peak = 0;
        for (let i = 0; i < 30; i++) { await sleep(50); peak = Math.max(peak, c.stats.bufferBytes); }
        a.ok(peak < 6 << 20, "client buffers stay bounded while 48 MiB wait on a stalled target: " + peak);
        f.close();
        c.close();
        await pending;
    }, { timeoutMs: 60000 });

    t.test("bytes left queued to a socket that never reads are dropped when the SSH connection ends", async (a) => {
        const burst = keep(burstTarget(12 << 20));
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 16 << 20 } }));
        const f = c.forwardLocal({ host: "127.0.0.1", port: burst.port });
        let sock = null;
        await new Promise((res) => TCPServer.connect({ host: "127.0.0.1", port: f.listenPort }, { connect: (conn) => { sock = conn; conn.pause(); conn.end(); res(); } }));
        for (let i = 0; i < 250 && c.stats.bytesIn < 12 << 20; i++) await sleep(20);
        for (let i = 0; i < 250 && c.stats.openChannels !== 0; i++) await sleep(20);
        await sleep(100);
        const during = acceptedSockets(f.listenPort);
        a.ok(c.connected && c.stats.openChannels === 0, "the channel is closed by both sides while its socket still holds unread bytes: " + c.stats.openChannels);
        c.close();
        await sleep(300);
        const after = acceptedSockets(f.listenPort);
        if (during !== null) {
            a.eq(during, 1, "while draining: the relay still holds the accepted socket");
            a.eq(after, 0, "the SSH connection ending closed the draining socket");
        }
        sock.close();
    }, { timeoutMs: 60000 });

    t.test("at most dyna.maxChannels sockets per connection keep draining after their channel closed", async (a) => {
        const burst = keep(burstTarget(12 << 20));
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 16 << 20, maxChannels: 2 } }));
        const f = c.forwardLocal({ host: "127.0.0.1", port: burst.port });
        const socks = [];
        for (let k = 1; k <= 3; k++) {
            await new Promise((res) => TCPServer.connect({ host: "127.0.0.1", port: f.listenPort }, { connect: (conn) => { socks.push(conn); conn.pause(); conn.end(); res(); } }));
            for (let i = 0; i < 250 && c.stats.bytesIn < k * (12 << 20); i++) await sleep(20);
            for (let i = 0; i < 250 && c.stats.openChannels !== 0; i++) await sleep(20);
            await sleep(100);
            a.ok(c.stats.openChannels === 0, "connection " + k + ": its channel closed while bytes stay queued");
        }
        const held = acceptedSockets(f.listenPort);
        if (held !== null) a.eq(held, 2, "the third draining socket was closed at once (cap = maxChannels = 2)");
        a.ok(c.connected, "the SSH connection is unaffected");
        c.close();
        await sleep(300);
        if (held !== null) a.eq(acceptedSockets(f.listenPort), 0, "the connection ending closed the two draining sockets");
        for (const x of socks) x.close();
    }, { timeoutMs: 60000 });

    t.test("a CLOSE that arrives while bytes are still queued to a slow socket loses nothing", async (a) => {
        const burst = keep(burstTarget(6 << 20));
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s));
        const f = c.forwardLocal({ host: "127.0.0.1", port: burst.port });
        const r = await exchange(f.listenPort, new Uint8Array(0), { slowMs: 2, timeoutMs: 60000 });
        let ok = r.bytes.byteLength === 6 << 20;
        for (let i = 0; ok && i < r.bytes.byteLength; i += 4099) if (r.bytes[i] !== ((i * 7) & 255)) ok = false;
        a.ok(ok, "6 MiB burst byte-exact to a slow reader: " + r.bytes.byteLength + " " + r.error);
        f.close();
        c.close();
    }, { timeoutMs: 90000 });

    t.test("forwardLocal: 8 MiB byte-exact, half-close, what the hook sees", async (a) => {
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s));
        const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
        a.eq(f.type, "local");
        a.eq(f.listenHost, "127.0.0.1");
        a.ok(f.listenPort > 0, "ephemeral local port");
        a.eq(f.targetPort, echo.port);
        const data = bytes(8 << 20, 3);
        const r = await exchange(f.listenPort, data);
        a.ok(same(r.bytes, data), "echo through direct-tcpip byte-exact: " + r.bytes.byteLength);
        const f2 = c.forwardLocal({ host: "127.0.0.1", port: half.port });
        const h = await exchange(f2.listenPort, bytes(1 << 20, 4));
        a.eq(text(h.bytes), "got 1048576", "the target saw the client's EOF and its answer came back");
        const last = seen[seen.length - 1];
        a.eq(last.type, "local");
        a.eq(last.host, "127.0.0.1");
        a.eq(last.port, half.port);
        a.eq(last.username, "alice");
        a.eq(last.originatorHost, "127.0.0.1");
        a.ok(last.originatorPort > 0);
        a.ok(last.keyFingerprint.startsWith("SHA256:"));
        a.eq(f.connections, 1);
        f.close();
        a.ok(f.closed);
        const after = await exchange(f.listenPort, bytes(10, 1), { timeoutMs: 3000 });
        a.ok(after.bytes.byteLength === 0, "the closed listener accepts nothing");
        f2.close();
        c.close();
    }, { timeoutMs: 90000 });

    t.test("forwardLocal: refusals (no hook, false, throw, non-boolean, async, unreachable)", async (a) => {
        const cases = [[undefined, false], [() => false, false], [() => { throw new Error("no"); }, false], [() => 1, false],
            [() => Promise.resolve(true), true], [() => Promise.resolve(false), false], [() => Promise.reject(new Error("x")), false]];
        for (const [hook, ok] of cases) {
            const s = listen(hook ? { forward: hook } : {});
            const c = await SSHClient.connect(base(s));
            const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
            const r = await exchange(f.listenPort, bytes(1000, 2), { timeoutMs: 5000 });
            a.eq(same(r.bytes, bytes(1000, 2)), ok, "hook " + String(hook) + " -> " + ok);
            a.ok(c.connected, "a refused channel leaves the connection up");
            c.close();
        }
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s));
        const dead = keep(echoTarget());
        const port = dead.port;
        dead.close();
        const f = c.forwardLocal({ host: "127.0.0.1", port });
        const r = await exchange(f.listenPort, bytes(10, 1), { timeoutMs: 5000 });
        a.eq(r.bytes.byteLength, 0, "an unreachable target closes the local socket");
        a.ok(c.connected);
        c.close();
    }, { timeoutMs: 90000 });

    t.test("forwardRemote: port 0, byte-exact, cancel, refusal, cap", async (a) => {
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s));
        const f = await c.forwardRemote({ host: "127.0.0.1", port: echo.port });
        a.eq(f.type, "remote");
        a.eq(f.listenHost, "127.0.0.1");
        a.ok(f.listenPort > 0, "the server allocated port " + f.listenPort);
        const req = seen[seen.length - 1];
        a.eq(req.type, "remote");
        a.eq(req.host, "127.0.0.1");
        a.eq(req.port, 0);
        const data = bytes(2 << 20, 9);
        const r = await exchange(f.listenPort, data);
        a.ok(same(r.bytes, data), "byte-exact through forwarded-tcpip");
        const h = await c.forwardRemote({ host: "127.0.0.1", port: half.port });
        a.eq(text((await exchange(h.listenPort, bytes(5000, 1))).bytes), "got 5000");
        const again = bytes(1000, 5);
        a.ok(same((await exchange(f.listenPort, again)).bytes, again), "each remote forward reaches its own target (matched by bound port)");
        a.eq(f.connections, 2);
        f.close();
        await sleep(100);
        const gone = await exchange(f.listenPort, bytes(10, 1), { timeoutMs: 3000 });
        a.eq(gone.bytes.byteLength, 0, "cancel-tcpip-forward closed the server listener");
        const more = [];
        for (let i = 0; i < 20; i++) more.push(c.forwardRemote({ host: "127.0.0.1", port: echo.port }).then(() => 1, () => 0));
        const ok = (await Promise.all(more)).reduce((x, y) => x + y, 0);
        a.eq(ok, 15, "16 remote forwards per connection (one already open)");
        c.close();
        const s2 = listen();
        const c2 = await SSHClient.connect(base(s2));
        const e = await c2.forwardRemote({ host: "127.0.0.1", port: echo.port }).catch((x) => x);
        a.ok(e instanceof Error && /refused/.test(e.message), "no hook: " + e);
        a.ok(c2.connected);
        c2.close();
    }, { timeoutMs: 90000 });

    t.test("remote binds: 127.0.0.1 unless dyna.publicForwardBinds", async (a) => {
        const lan = lanAddress();
        for (const pub of [false, true]) {
            const s = listen({ forward: allow, dyna: { publicForwardBinds: pub } });
            const c = await SSHClient.connect(base(s));
            const f = await c.forwardRemote({ host: "127.0.0.1", port: echo.port, remoteHost: "0.0.0.0" });
            a.eq(f.listenHost, "0.0.0.0");
            const lo = await exchange(f.listenPort, bytes(100, 3));
            a.ok(same(lo.bytes, bytes(100, 3)), "loopback reaches the forward");
            const bound = boundAddress(f.listenPort);
            if (bound !== null)
                a.eq(bound, pub ? "*" : "127.0.0.1", "listener bound to " + bound + " with publicForwardBinds " + pub);
            else if (lan) {
                const r = await exchange(f.listenPort, bytes(100, 3), { host: lan, timeoutMs: 3000 });
                a.eq(same(r.bytes, bytes(100, 3)), pub, "via " + lan + " with publicForwardBinds " + pub);
            }
            c.close();
        }
    }, { timeoutMs: 60000 });

    t.test("a certificate without permit-port-forwarding refuses both directions", async (a) => {
        const ca = keygen(dir + "/ca2");
        sys.Exec("cp", [dir + "/user", dir + "/cu"]);
        sys.Exec("cp", [dir + "/user.pub", dir + "/cu.pub"]);
        const cert = certify(dir, dir + "/ca2", dir + "/cu.pub", { principals: "alice", options: ["no-port-forwarding"] });
        const s = keep(SSHServer.listen({ hostKey: host.priv, trustedUserCAKeys: ca.pub, forward: allow, onSession: () => {} }));
        const c = await SSHClient.connect(base(s, { certificate: cert }));
        const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
        a.eq((await exchange(f.listenPort, bytes(10, 1), { timeoutMs: 5000 })).bytes.byteLength, 0, "local refused");
        a.ok((await c.forwardRemote({ host: "127.0.0.1", port: echo.port }).catch((e) => e)) instanceof Error, "remote refused");
        c.close();
        const cert2 = certify(dir, dir + "/ca2", dir + "/cu.pub", { principals: "alice" });
        const c2 = await SSHClient.connect(base(s, { certificate: cert2 }));
        const f2 = c2.forwardLocal({ host: "127.0.0.1", port: echo.port });
        a.ok(same((await exchange(f2.listenPort, bytes(10, 1))).bytes, bytes(10, 1)), "a default certificate permits forwarding");
        c2.close();
    }, { timeoutMs: 60000 });

    t.test("ProxyJump: connect({via}) checks the target's own host key", async (a) => {
        const jump = listen({ forward: allow });
        const target = listen({}, host2);
        const outer = await SSHClient.connect(base(jump));
        const inner = await SSHClient.connect({ ...base(target), knownHosts: kh(target, host2), via: outer });
        a.ok(inner.connected);
        a.eq(text((await run(inner, "x")).stdout), "hello x\n");
        const last = seen[seen.length - 1];
        a.eq(last.port, target.port, "the jump saw a direct-tcpip to the target");
        const big = await run(inner, "y");
        a.eq(text(big.stdout), "hello y\n");
        inner.close();
        await sleep(50);
        a.ok(outer.connected, "closing the inner client leaves the jump up");
        a.eq(text((await run(outer, "z")).stdout), "hello z\n");
        const bad = await SSHClient.connect({ ...base(target), knownHosts: kh(target, host), via: outer }).catch((e) => e);
        a.ok(bad instanceof Error && /host key/i.test(bad.message), "target key mismatch: " + bad);
        const inner2 = await SSHClient.connect({ ...base(target), knownHosts: kh(target, host2), via: outer });
        const jump2 = listen({ forward: allow });
        const hop2 = await SSHClient.connect({ ...base(jump2), via: outer });
        const hop3 = await SSHClient.connect({ ...base(target), knownHosts: kh(target, host2), via: hop2 });
        a.eq(text((await run(hop3, "two-hop")).stdout), "hello two-hop\n", "a via chain nests: client -> jump -> jump2 -> target");
        outer.close();
        for (let i = 0; i < 50 && hop3.connected; i++) await sleep(20);
        a.ok(!hop2.connected && !hop3.connected, "closing the first jump ends every hop after it");
        for (let i = 0; i < 50 && inner2.connected; i++) await sleep(20);
        a.ok(!inner2.connected, "closing the jump ends the inner client");
        const refused = listen();
        const o2 = await SSHClient.connect(base(refused));
        const e = await SSHClient.connect({ ...base(target), knownHosts: kh(target, host2), via: o2 }).catch((x) => x);
        a.ok(e instanceof Error && /jump/.test(e.message), "a jump without a forward hook refuses: " + e);
        o2.close();
    }, { timeoutMs: 60000 });

    t.test("closing the connection closes its listeners; fds stay flat", async (a) => {
        const s = listen({ forward: allow });
        let fd0 = 0;
        for (let i = 0; i < 6; i++) {
            if (i === 1) { await sleep(100); fd0 = fdCount(); }
            const c = await SSHClient.connect(base(s));
            const l = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
            const rf = await c.forwardRemote({ host: "127.0.0.1", port: echo.port });
            a.ok(same((await exchange(l.listenPort, bytes(1000, i))).bytes, bytes(1000, i)));
            a.ok(same((await exchange(rf.listenPort, bytes(1000, i))).bytes, bytes(1000, i)));
            c.close();
            await sleep(100);
            a.ok(l.closed && rf.closed, "forwards report closed");
            a.eq((await exchange(rf.listenPort, bytes(5, 1), { timeoutMs: 3000 })).bytes.byteLength, 0, "server listener gone");
            a.eq((await exchange(l.listenPort, bytes(5, 1), { timeoutMs: 3000 })).bytes.byteLength, 0, "local listener gone");
        }
        await sleep(100);
        a.ok(fdCount() <= fd0, "fd count after five more rounds " + fdCount() + " <= " + fd0);
    }, { timeoutMs: 90000 });

    t.test("option validation", async (a) => {
        const s = listen({ forward: allow });
        const c = await SSHClient.connect(base(s));
        a.throws(() => c.forwardLocal(), TypeError);
        a.throws(() => c.forwardLocal({ port: 1 }), /host is required/);
        a.throws(() => c.forwardLocal({ host: "x" }), /port is required/);
        a.throws(() => c.forwardLocal({ host: "x", port: 70000 }), RangeError);
        a.throws(() => c.forwardLocal({ host: "x", port: 1, remoteHost: "y" }), TypeError);
        a.throws(() => c.forwardLocal({ host: "x", port: 1, bogus: 1 }), TypeError);
        a.throws(() => c.forwardRemote({ host: "x", port: 1, localPort: 5 }), TypeError);
        a.throws(() => SSHClient.connect({ ...base(s), via: {} }), /via must be an SSHClient/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, forward: 5 }), /must be a function/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, dyna: { publicForwardBinds: 1 } }), /publicForwardBinds must be a boolean/);
        c.close();
        a.throws(() => c.forwardLocal({ host: "x", port: 1 }), Error);
    });
}

await t.run();
