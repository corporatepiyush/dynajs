// flags: --std
// covers: integration dyna:sftp.SFTPServer dyna:sftp.SFTPClient -- hostile bytes into both C parsers, and process teardown with live SFTP state
// timeout: 120
// Server parser: an exec session handed to SFTPServer.serve receives raw bytes written by SSHExec.write. A zero, short or over-256 KiB
// length, a first packet that is not INIT and a truncated request end the session with serve() reporting the error; a handle of the
// wrong size, a NUL in a path, a 4 GiB READ length and an unknown type each get a STATUS and the session goes on; a request split
// into 1-byte writes is answered; six 255 KiB READs sent in one write and then nothing more are all answered (the server resumes
// past its 256 KiB output high-water mark from the channel's written event). Client parser: an "sftp" subsystem session scripted by onSession answers with version 2, an
// oversized length, a non-VERSION first packet, a HANDLE over 256 bytes, a NAME count of a million, DATA longer than the READ asked
// for, and a reply for an unknown id followed by the real one. Teardown (sftp_teardown_child.js, run with $BIN): exiting or throwing
// with an open SFTPClient, a pending read() on an unclosed SFTPFile, and a server session parked on an allow Promise, and
// SSHServer.close() / SSHClient.close() in the middle of a transfer, exit with the script's code and no sanitizer report.
import * as std from "std";
import * as sys from "dyna:sys";
import { SSHServer, SSHClient } from "dyna:ssh";
import { SFTPServer, SFTPClient } from "dyna:sftp";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir, keygen, write, pubBlob, sleep } from "../ssh/ssh_util.js";
import { bytes, makeTree, sftpHost, writeBin } from "./sftp_util.js";

const t = suite("sftp_server_malformed");
const closers = [];
t.after(() => { for (const c of closers) { try { c(); } catch {} } });
const te = new TextEncoder();

function cat(...parts) {
    let n = 0;
    for (const p of parts) n += p.length;
    const o = new Uint8Array(n);
    let k = 0;
    for (const p of parts) { o.set(p, k); k += p.length; }
    return o;
}
const u8 = (v) => Uint8Array.of(v);
const u32 = (v) => Uint8Array.of(v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const u64 = (v) => cat(u32(Math.floor(v / 4294967296)), u32(v >>> 0));
const str = (s) => { const b = typeof s === "string" ? te.encode(s) : s; return cat(u32(b.length), b); };
const pkt = (type, ...parts) => { const body = cat(u8(type), ...parts); return cat(u32(body.length), body); };

function parse(buf) {
    const out = [];
    let o = 0;
    while (o + 4 <= buf.length) {
        const L = (buf[o] << 24 | buf[o + 1] << 16 | buf[o + 2] << 8 | buf[o + 3]) >>> 0;
        if (o + 4 + L > buf.length) break;
        const b = buf.subarray(o + 4, o + 4 + L);
        const r = { type: b[0], id: (b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]) >>> 0, body: b.slice() };
        if (r.type === 101) r.code = (b[5] << 24 | b[6] << 16 | b[7] << 8 | b[8]) >>> 0;
        out.push(r);
        o += 4 + L;
    }
    return out;
}

async function drain(rs, ms) {
    const r = rs.getReader();
    const parts = [];
    const end = Date.now() + ms;
    for (;;) {
        const left = end - Date.now();
        if (left <= 0) break;
        const x = await Promise.race([r.read(), sleep(left).then(() => ({ timeout: true }))]);
        if (x.timeout || x.done) break;
        parts.push(x.value);
    }
    r.releaseLock();
    return cat(...parts);
}

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const tree = makeTree("sftpmal");
    const dir = tmpDir("sftpmal");
    const hk = keygen(dir + "/host"), uk = keygen(dir + "/user");
    const results = [];
    let script = null;
    const srv = SSHServer.listen({ hostKey: hk.priv, authorizedKeys: uk.pub, onSession: (s) => {
        if (s.type === "exec") {
            const p = SFTPServer.serve(s, { root: tree.root });
            p.then((r) => results.push(r));
            return p;
        }
        return script(s);
    } });
    closers.push(() => srv.close());
    write(dir + "/kh", "[127.0.0.1]:" + srv.port + " " + pubBlob(hk.pub) + "\n");
    const base = { host: "127.0.0.1", port: srv.port, username: "u", privateKey: uk.priv, knownHosts: dir + "/kh" };
    const INIT = pkt(1, u32(3));

    async function rawSession(chunks, ms = 400) {
        const ssh = await SSHClient.connect(base);
        const x = await ssh.exec("sftp-raw");
        const before = results.length;
        for (const c of chunks) await x.write(c);
        const out = await drain(x.stdout, ms);
        try { x.end(); } catch {}
        for (let i = 0; i < 50 && results.length === before; i++) await sleep(10);
        ssh.close();
        return { replies: parse(out), result: results[before] };
    }

    t.test("server: framing errors end the session with an error", async (a) => {
        for (const [name, bytes_] of [["zero length", cat(u32(0))], ["length 3", cat(u32(3), u8(1), u8(0), u8(0))],
            ["over 256 KiB", cat(u32((256 << 10) + 1), u8(1))], ["first packet not INIT", pkt(17, u32(1), str("x"))],
            ["truncated OPEN", cat(INIT, pkt(3, u32(1), u32(50)))]]) {
            const r = await rawSession([bytes_]);
            a.ok(r.result && /malformed|oversized/.test(r.result.error || ""), name + ": " + JSON.stringify(r.result));
        }
    });

    t.test("server: bad fields get a STATUS and the session continues", async (a) => {
        const r = await rawSession([INIT,
            pkt(5, u32(1), str(new Uint8Array(3)), u64(0), u32(10)),
            pkt(17, u32(2), str(cat(te.encode("hel"), u8(0), te.encode("lo.txt")))),
            pkt(99, u32(3)),
            pkt(3, u32(4), str("hello.txt"), u32(1), u32(0)),
            pkt(4, u32(5), str(new Uint8Array(300))),
            pkt(17, u32(6), str("hello.txt"))]);
        const byId = Object.fromEntries(r.replies.map((x) => [x.id, x]));
        a.eq(r.replies[0].type, 2, "VERSION");
        a.eq(byId[1].code, 4, "READ with a 3-byte handle: FAILURE");
        a.eq(byId[2].code, 4, "a NUL in a path: FAILURE");
        a.eq(byId[3].code, 8, "unknown type: OP_UNSUPPORTED");
        a.eq(byId[4].type, 102, "OPEN still works");
        a.eq(byId[5].code, 4, "CLOSE with a 300-byte handle: FAILURE");
        a.eq(byId[6].type, 105, "STAT after all that: ATTRS");
        a.eq(r.result.error, null);
    });

    t.test("server: READs queued past the output high-water mark are all answered with no further input", async (a) => {
        writeBin(tree.root + "/big.bin", bytes(6 * 261120, 21));
        const ssh = await SSHClient.connect(base);
        const x = await ssh.exec("sftp-raw");
        await x.write(cat(INIT, pkt(3, u32(1), str("big.bin"), u32(1), u32(0))));
        let got = parse(await drain(x.stdout, 400));
        const hr = got.find((r) => r.type === 102);
        a.ok(hr, "OPEN answered");
        const hl = (hr.body[5] << 24 | hr.body[6] << 16 | hr.body[7] << 8 | hr.body[8]) >>> 0;
        const handle = hr.body.slice(9, 9 + hl);
        const reads = [];
        for (let i = 0; i < 6; i++) reads.push(pkt(5, u32(10 + i), str(handle), u64(i * 261120), u32(261120)));
        await x.write(cat(...reads));
        got = parse(await drain(x.stdout, 1500));
        a.eq(got.filter((r) => r.type === 103).length, 6, "six DATA replies (1.5 MiB, under the 2 MiB window) after one write: " + got.map((r) => r.type + ":" + r.id).join(","));
        try { x.end(); } catch {}
        ssh.close();
    });

    t.test("server: a 4 GiB READ length is capped; 1-byte writes are reassembled", async (a) => {
        const open = pkt(3, u32(1), str("hello.txt"), u32(1), u32(0));
        const chunks = [INIT, open];
        const r1 = await rawSession(chunks);
        a.eq(r1.replies[1].type, 102);
        const one = [];
        for (const b of cat(INIT, pkt(17, u32(7), str("hello.txt")))) one.push(u8(b));
        const r2 = await rawSession(one, 800);
        a.eq(r2.replies.length, 2);
        a.eq(r2.replies[1].type, 105);
        const big = new Uint8Array(300000);
        big.fill(65);
        write(tree.root + "/big.txt", new TextDecoder().decode(big));
        const ssh = await SSHClient.connect(base);
        const x = await ssh.exec("sftp-raw");
        await x.write(INIT);
        await x.write(pkt(3, u32(1), str("big.txt"), u32(1), u32(0)));
        const first = parse(await drain(x.stdout, 300));
        a.eq(first[1].type, 102);
        const hb = first[1].body, hl = (hb[5] << 24 | hb[6] << 16 | hb[7] << 8 | hb[8]) >>> 0;
        await x.write(pkt(5, u32(2), str(hb.subarray(9, 9 + hl)), u64(0), u32(0xFFFFFFFF)));
        const d = parse(await drain(x.stdout, 300));
        a.eq(d[0].type, 103);
        a.eq((d[0].body[5] << 24 | d[0].body[6] << 16 | d[0].body[7] << 8 | d[0].body[8]) >>> 0, 261120, "READ of 2^32-1 bytes answered with 261120");
        ssh.close();
    });

    function scripted(reply) {
        script = (s) => {
            const r = s.stdin.getReader();
            let buf = new Uint8Array(0), inited = false;
            (async () => {
                for (;;) {
                    const { value, done } = await r.read();
                    if (done) break;
                    buf = cat(buf, value);
                    for (;;) {
                        if (buf.length < 4) break;
                        const L = (buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3]) >>> 0;
                        if (buf.length < 4 + L) break;
                        const p = buf.subarray(4, 4 + L);
                        buf = buf.slice(4 + L);
                        if (!inited) { inited = true; await s.write(reply.version || pkt(2, u32(3))); continue; }
                        const id = (p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4]) >>> 0;
                        await s.write(reply.answer(p[0], id));
                    }
                }
            })().catch(() => {});
            return s.done;
        };
    }

    async function clientTry(fn) {
        const ssh = await SSHClient.connect(base);
        try { return await fn(ssh); } catch (e) { return e; } finally { ssh.close(); }
    }

    t.test("client: bad VERSION replies refuse the session", async (a) => {
        for (const [name, v, re] of [["version 2", pkt(2, u32(2)), /older than 3/], ["2 MiB length", cat(u32(2 << 20), u8(2)), /malformed or oversized/],
            ["STATUS first", pkt(101, u32(0), u32(4), str("x"), str("")), /did not answer the SFTP init/]]) {
            scripted({ version: v, answer: () => new Uint8Array(0) });
            const e = await clientTry((ssh) => SFTPClient.open(ssh));
            a.ok(e instanceof Error && re.test(e.message), name + ": " + e);
        }
    });

    t.test("client: hostile replies reject the request, not the process", async (a) => {
        scripted({ answer: (type, id) => {
            if (type === 3) return pkt(102, u32(id), str(new Uint8Array(300)));
            if (type === 11) return pkt(102, u32(id), str("h"));
            if (type === 12) return pkt(104, u32(id), u32(1000000));
            if (type === 17) return cat(pkt(105, u32(id + 1000), u32(0)), pkt(105, u32(id), u32(1), u64(42)));
            return pkt(101, u32(id), u32(4), str("no"), str(""));
        } });
        const r = await clientTry(async (ssh) => {
            const sftp = await SFTPClient.open(ssh);
            const out = {};
            out.open = await sftp.open("x").catch((e) => e);
            out.readdir = await sftp.readdir("d").catch((e) => e);
            out.stat = await sftp.stat("s");
            await sftp.close();
            return out;
        });
        a.eq(r.open.code, "BAD_MESSAGE", "HANDLE over 256 bytes");
        a.eq(r.readdir.code, "BAD_MESSAGE", "NAME count of a million");
        a.eq(r.stat.size, 42, "an unknown id is ignored and the real reply lands");
        scripted({ answer: (type, id) => {
            if (type === 3) return pkt(102, u32(id), str("h"));
            if (type === 5) return pkt(103, u32(id), str(new Uint8Array(40000)));
            return pkt(101, u32(id), u32(0), str(""), str(""));
        } });
        const e = await clientTry(async (ssh) => (await SFTPClient.open(ssh, { dyna: { chunkBytes: 32768, maxInflight: 2 } })).readFile("x"));
        a.eq(e.code, "BAD_MESSAGE", "DATA longer than the READ asked for");
    });

    t.test("process teardown with live SFTP state exits cleanly", async (a) => {
        const bin = std.getenv("BIN") || "./dynajs";
        for (const [mode, rc] of [["client-throw", 1], ["client-exit", 3], ["read-pending-throw", 1], ["hook-pending-throw", 1], ["server-close", 0], ["ssh-close", 0]]) {
            const r = sys.Exec("/bin/sh", ["-c", bin + " --std tests/sftp/sftp_teardown_child.js " + mode + " </dev/null 2>&1"]);
            a.eq(r.code, rc, mode + " exit code: " + r.stdout.slice(-400));
            a.ok(!/Assertion failed|AddressSanitizer|runtime error|LeakSanitizer/.test(r.stdout), mode + " teardown clean");
        }
    }, { timeoutMs: 90000 });
}

await t.run();
