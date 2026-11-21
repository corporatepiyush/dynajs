// flags: --std
// covers: contract dyna:net.TCPServer dyna:dns.DNSResolver dyna:postgres.PostgreSQL dyna:redis.Redis dyna:smtp.SmtpClient dyna:http.HTTPClient dyna:ssh.SSHClient dyna:sftp.SFTPClient dyna:ai.Anthropic dyna:ai.OpenAI dyna:scrape.Fetcher dyna:sys.Spawn dyna:docdb.Database dyna:webrtc.RTCPeerConnection -- every client must keep itself alive while work is in flight and stay collectable once idle
// F-DNS-TTL-HANG class: a client dropped with work in flight was collected with its pending resolve/reject and the awaiting
// frame (one unreachable cycle), so the promise never settled and the loop hung. Each case drops every handle inside a
// frame that only the client can resume, forces GCs from timers that reference nothing, and asserts the work still settles
// (the kit's timeoutMs fails a lost one). Fake peers run in-process; postgres and redis come from tests/lib/services.js.
import * as std from "std";
import { TCPServer, UDPSocket } from "dyna:net";
import { DNSResolver } from "dyna:dns";
import { PostgreSQL } from "dyna:postgres";
import { Redis } from "dyna:redis";
import { SmtpClient } from "dyna:smtp";
import { HTTPClient } from "dyna:http";
import { SSHClient } from "dyna:ssh";
import { SFTPClient } from "dyna:sftp";
import { Anthropic, OpenAI } from "dyna:ai";
import { Fetcher } from "dyna:scrape";
import { Spawn } from "dyna:sys";
import { Database } from "dyna:docdb";
import { RTCPeerConnection } from "dyna:webrtc";
import { suite } from "../lib/kit.js";
import { ensurePostgres, ensureRedis, stopStarted, PG_PORT, REDIS_PORT } from "../lib/services.js";
import { HAVE_KEYGEN, tmpDir } from "../ssh/ssh_util.js";
import { sftpHost, makeTree } from "../sftp/sftp_util.js";

const t = suite("test_net_inflight_gc");
const HOST = "127.0.0.1", TMO = { timeoutMs: 4000 };
const PG = { host: HOST, port: PG_PORT, user: "postgres", password: "pw", database: "postgres" };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let pg = null, redis = null, ssh = null, dir = null;

function gcStorm() {
    std.gc();
    for (const ms of [0, 2, 5, 10, 20, 40, 90, 200, 400]) setTimeout(() => std.gc(), ms);
}

async function settle(op) {
    try { const v = await op(); return { ok: true, v }; }
    catch (e) { return { ok: false, msg: String(e && e.message || e) }; }
}

async function viaFake(mk, closeAfterMs) {
    const srv = new TCPServer({ port: 0 });
    const conns = [];
    srv.start({ connect: (c) => { if (c) conns.push(c); }, data: () => {} });
    const closer = closeAfterMs ? setTimeout(() => { for (const c of conns) c.close(); std.gc(); }, closeAfterMs) : null;
    gcStorm();
    try { return await settle(() => mk(srv.port, "http://" + HOST + ":" + srv.port + "/")); }
    finally { if (closer) clearTimeout(closer); srv.close(); }
}

function rejects(a, r, re, what) {
    a.ok(!r.ok, what + ": rejected", r.ok ? "fulfilled" : "");
    if (!r.ok) a.match(r.msg, re, what + ": message");
}

async function collected(make) {
    let fired = 0;
    const fr = new FinalizationRegistry(() => { fired++; });
    await (async () => { fr.register(await make(), 1); })();
    for (let i = 0; i < 75 && !fired; i++) { await sleep(20); std.gc(); }
    return fired;
}

t.before(async () => {
    pg = await ensurePostgres({ port: PG_PORT });
    redis = await ensureRedis(REDIS_PORT);
    dir = tmpDir("inflight-gc");
    if (HAVE_KEYGEN) {
        const tree = makeTree("inflight-gc-sftp");
        ssh = sftpHost(tree.base, { root: tree.root });
    }
});
t.after(async () => { if (ssh) ssh.close(); await stopStarted(); });

const needPg = (a) => { if (!pg || !pg.ok) a.skip("postgres: " + (pg && pg.why || "unavailable")); };
const needRedis = (a) => { if (!redis || !redis.ok) a.skip("redis: " + (redis && redis.why || "unavailable")); };
const needSsh = (a) => { if (!ssh) a.skip("no ssh-keygen on this host"); };

t.test("dns: a dropped resolver times out against a silent server after a GC", async (a) => {
    const u = new UDPSocket({ port: 0, host: HOST });
    u.start({ message: () => {} });
    gcStorm();
    const r = await settle(() => new DNSResolver({ server: HOST, port: u.port, timeoutMs: 300 }).lookup("silent.test"));
    u.close();
    rejects(a, r, /query timed out/, "lookup");
}, TMO);

t.test("postgres: a dropped client mid-startup rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port) => new PostgreSQL({ ...PG, port, connectTimeoutMs: 60000 }).query("select 1"), 250), /server closed the connection/, "query");
}, TMO);

t.test("postgres: a dropped client mid-startup rejects on its connect timer", async (a) => {
    rejects(a, await viaFake((port) => new PostgreSQL({ ...PG, port, connectTimeoutMs: 300 }).query("select 1"), 0), /connect timed out/, "query");
}, TMO);

t.test("redis: a dropped client mid-handshake rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port) => new Redis({ host: HOST, port, connectTimeoutMs: 60000, commandTimeoutMs: 60000 }).command("PING"), 250), /server closed the connection/, "command");
}, TMO);

t.test("redis: a dropped client mid-handshake rejects on its connect timer", async (a) => {
    rejects(a, await viaFake((port) => new Redis({ host: HOST, port, connectTimeoutMs: 300 }).command("PING"), 0), /connect timed out/, "command");
}, TMO);

t.test("smtp: a dropped client mid-greeting rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port) => new SmtpClient({ host: HOST, port, tls: "none" }).send({ from: "a@b.test", to: "c@d.test", data: "x\r\n" }), 250), /closed the connection/, "send");
}, TMO);

t.test("http: a dropped HTTPClient rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port, url) => new HTTPClient().get(url), 250), /connection closed/, "get");
}, TMO);

t.test("http: fetch rejects when the peer closes after a GC", async (a) => {
    rejects(a, await viaFake((port, url) => fetch(url), 250), /connection closed/, "fetch");
}, TMO);

t.test("http: a dropped WebSocket fires close when the peer closes", async (a) => {
    const r = await viaFake((port) => new Promise((res) => { new WebSocket("ws://" + HOST + ":" + port + "/", { close: (w, code) => res(code) }); }), 250);
    a.ok(r.ok, "close fired");
}, TMO);

t.test("net: a dropped TCPServer.connect client fires close when the peer closes", async (a) => {
    const r = await viaFake((port) => new Promise((res) => { TCPServer.connect({ host: HOST, port }, { connect: (c, e) => { if (e) res(e); }, close: () => res("close") }); }), 250);
    a.eq(r.ok && r.v, "close");
}, TMO);

t.test("ssh: a dropped SSHClient.connect rejects when the peer closes mid-handshake", async (a) => {
    needSsh(a);
    rejects(a, await viaFake((port) => SSHClient.connect({ ...ssh.base, port }), 250), /server closed the connection/, "connect");
}, TMO);

t.test("ai: a dropped Anthropic client rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port, url) => new Anthropic({ apiKey: "k", baseURL: url, maxRetries: 0 }).messages.create({ model: "m", max_tokens: 1, messages: [{ role: "user", content: "x" }] }), 250), /failed/, "create");
}, TMO);

t.test("ai: a dropped OpenAI client rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port, url) => new OpenAI({ apiKey: "k", baseURL: url, maxRetries: 0 }).chat.completions.create({ model: "m", messages: [{ role: "user", content: "x" }] }), 250), /failed/, "create");
}, TMO);

t.test("scrape: a dropped Fetcher rejects when the peer closes", async (a) => {
    rejects(a, await viaFake((port, url) => new Fetcher({ agent: "inflight-gc", allowPrivateHosts: true, robots: false, retries: 0 }).get(url), 250), /connection closed/, "get");
}, TMO);

t.test("postgres: a dropped live client finishes a query in flight", async (a) => {
    needPg(a);
    const r = await settle(async () => {
        const c = new PostgreSQL(PG);
        await c.query("select 1");
        const p = c.query("select 7 as n from pg_sleep(0.3)");
        gcStorm();
        return await p;
    });
    a.ok(r.ok, "query fulfilled", r.msg);
}, TMO);

t.test("postgres: a dropped live client queried before ready finishes", async (a) => {
    needPg(a);
    gcStorm();
    a.ok((await settle(() => new PostgreSQL(PG).query("select pg_sleep(0.3)"))).ok, "query fulfilled");
}, TMO);

t.test("postgres: a dropped live client rejects on queryTimeoutMs", async (a) => {
    needPg(a);
    const r = await settle(async () => {
        const c = new PostgreSQL({ ...PG, queryTimeoutMs: 200 });
        await c.query("select 1");
        const p = c.query("select pg_sleep(2)");
        gcStorm();
        return await p;
    });
    rejects(a, r, /query timed out/, "query");
}, TMO);

t.test("redis: a dropped live client finishes a blocking command in flight", async (a) => {
    needRedis(a);
    const r = await settle(async () => {
        const c = new Redis({ host: HOST, port: REDIS_PORT });
        await c.command("PING");
        const p = c.command("BLPOP", "inflight-gc:none", "0.3");
        gcStorm();
        return await p;
    });
    a.ok(r.ok, "BLPOP fulfilled", r.msg);
}, TMO);

t.test("redis: a dropped live client commanded before ready finishes", async (a) => {
    needRedis(a);
    gcStorm();
    a.ok((await settle(() => new Redis({ host: HOST, port: REDIS_PORT }).command("BLPOP", "inflight-gc:none", "0.3"))).ok, "BLPOP fulfilled");
}, TMO);

t.test("redis: a dropped live client rejects on commandTimeoutMs", async (a) => {
    needRedis(a);
    const r = await settle(async () => {
        const c = new Redis({ host: HOST, port: REDIS_PORT, commandTimeoutMs: 200 });
        await c.command("PING");
        const p = c.command("BLPOP", "inflight-gc:none", "2");
        gcStorm();
        return await p;
    });
    rejects(a, r, /command timed out/, "BLPOP");
}, TMO);

t.test("ssh: a dropped client finishes connect and exec", async (a) => {
    needSsh(a);
    const r = await settle(async () => {
        const p = SSHClient.connect(ssh.base);
        gcStorm();
        const s = await p;
        const e = s.exec("true");
        gcStorm();
        return await e;
    });
    a.ok(r.ok, "exec accepted", r.msg);
}, TMO);

t.test("sftp: a dropped client finishes open, stat and a file read", async (a) => {
    needSsh(a);
    const r = await settle(async () => {
        const op = SFTPClient.open(await SSHClient.connect(ssh.base));
        gcStorm();
        const f = await op;
        const st = f.stat("hello.txt");
        gcStorm();
        await st;
        const fh = await f.open("hello.txt");
        const rd = fh.read();
        gcStorm();
        return new TextDecoder().decode(await rd);
    });
    a.eq(r.ok && r.v, "hello world\n", r.msg);
}, TMO);

t.test("sys: a dropped Spawn still settles wait() and a stdout read", async (a) => {
    gcStorm();
    const w = await settle(() => new Spawn("sleep", ["0.3"]).wait());
    a.ok(w.ok, "wait settled", w.msg);
    gcStorm();
    const rd = await settle(() => new Spawn("sh", ["-c", "sleep 0.3; echo hi"]).stdout.read(new Uint8Array(16)));
    a.ok(rd.ok, "read settled", rd.msg);
}, TMO);

t.test("docdb: a dropped Database finishes persist and load off the JS thread", async (a) => {
    const path = dir + "/snap.db";
    gcStorm();
    const p = await settle(() => { const d = new Database("p"); d.collection("c").insertOne({ a: 1 }); return d.persist(path); });
    a.ok(p.ok, "persist settled", p.msg);
    gcStorm();
    a.ok((await settle(() => new Database("q").load(path))).ok, "load settled");
    gcStorm();
    a.ok((await settle(() => Database.load(path))).ok, "static load settled");
}, TMO);

t.test("webrtc: a dropped peer connection settles createOffer", async (a) => {
    gcStorm();
    const r = await settle(() => { const pc = new RTCPeerConnection(); pc.createDataChannel("x"); return pc.createOffer(); });
    a.ok(r.ok, "createOffer settled", r.msg);
}, TMO);

t.test("idle: a dropped resolver with every query settled is collected", async (a) => {
    a.eq(await collected(() => new DNSResolver({ server: HOST, port: 9 })), 1);
}, TMO);

t.test("idle: a dropped SFTP file whose read settled is collected while its session lives", async (a) => {
    needSsh(a);
    const f = await SFTPClient.open(await SSHClient.connect(ssh.base));
    a.eq(await collected(async () => { const fh = await f.open("hello.txt"); await fh.read(); return fh; }), 1);
    await f.close();
}, TMO);

t.test("idle: a dropped HTTPClient is collected", async (a) => {
    a.eq(await collected(() => new HTTPClient()), 1);
}, TMO);

t.test("idle: a dropped ready postgres client is collected", async (a) => {
    needPg(a);
    a.eq(await collected(async () => { const c = new PostgreSQL(PG); await c.query("select 1"); return c; }), 1);
}, TMO);

t.test("idle: a dropped ready redis client is collected", async (a) => {
    needRedis(a);
    a.eq(await collected(async () => { const c = new Redis({ host: HOST, port: REDIS_PORT }); await c.command("PING"); return c; }), 1);
}, TMO);

await t.run();
