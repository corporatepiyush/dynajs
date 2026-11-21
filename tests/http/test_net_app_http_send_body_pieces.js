// covers: c:src/dyna-aio.c
// An App response over 1 KiB used to be assembled into a temporary buffer (head, then body,
// growing it) and that buffer was copied again into the connection's hold buffer: two copies of
// every large body (memmove was 13% of a 60 KB-response server profile). dyn_aio_send2 now takes
// head and body separately and, when the response goes to the hold buffer (inside a dispatch,
// nothing queued, head+body <= 64 KiB), copies both straight into it as ONE piece with one
// completion; otherwise (TLS, a queued write, outside a dispatch, or bigger) it concatenates and
// sends exactly as before. These cases pin the bytes on the wire: pipelined bursts that mix sizes
// on both sides of the 1 KiB and 64 KiB limits, answers from async handlers (sent outside the
// dispatch), string (ASCII and UTF-8) and byte bodies, small
// error answers between large ones, and status/headers, each body position-patterned so a swapped, dropped or doubled piece shows.
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";

const t = suite("net_app_http_send_body_pieces");

const pat = (n, salt) => {
    let s = "";
    let i = 0;
    while (s.length < n) { s += salt + (i++).toString(36) + "|"; }
    return s.slice(0, n);
};
const utf8 = (s) => { let o = ""; for (const ch of s) { const c = ch.codePointAt(0); if (c < 0x80) o += ch; else if (c < 0x800) o += String.fromCharCode(0xc0 | (c >> 6), 0x80 | (c & 63)); else o += String.fromCharCode(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); } return o; };
const latin1 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192)); return s; };
const bytes = (s) => { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff; return a; };

const app = new App({ port: 0, compress: false });
app.get("/s/:n", (r) => pat(+r.params.n, "s" + r.params.n + "."));
app.get("/u/:n", (r) => pat(+r.params.n, "é" + r.params.n + "世"));
app.get("/b/:n", (r) => bytes(pat(+r.params.n, "b" + r.params.n + ".")));
app.get("/a/:n", async (r) => { await new Promise((res) => setTimeout(res, 1)); return pat(+r.params.n, "a" + r.params.n + "."); });
app.get("/st/:n", (r) => ({ status: 201, headers: { "X-K": "v" + r.params.n }, body: pat(+r.params.n, "t" + r.params.n + ".") }));
app.start();
const port = app.port;

function parseAll(buf) {
    const out = [];
    for (;;) {
        const he = buf.indexOf("\r\n\r\n");
        if (he < 0) break;
        const head = buf.slice(0, he);
        const m = /Content-Length: (\d+)/i.exec(head);
        const blen = m ? Number(m[1]) : 0;
        if (buf.length < he + 4 + blen) break;
        out.push({ head, cl: m ? Number(m[1]) : -1, body: buf.substr(he + 4, blen) });
        buf = buf.slice(he + 4 + blen);
    }
    return { out, rest: buf };
}

function burst(reqs) {
    return new Promise((resolve) => {
        let buf = "", got = [], timer;
        const finish = (why) => { clearTimeout(timer); resolve({ got, why, rest: buf }); };
        timer = setTimeout(() => finish("timeout"), 20000);
        TCPServer.connect({ host: "127.0.0.1", port }, {
            connect: (c, err) => {
                if (err) return finish("connect " + err);
                c.write(bytes(reqs.map(([m, p]) => `${m} ${p} HTTP/1.1\r\nHost: x\r\n\r\n`).join("")));
            },
            data: (c, b) => {
                buf += latin1(b);
                const r = parseAll(buf);
                buf = r.rest;
                for (const x of r.out) got.push(x);
                if (got.length >= reqs.length) { c.close(); finish("done"); }
            },
            close: () => finish("closed"),
        });
    });
}

const want = (m, p) => {
    const [, kind, n] = /^\/(\w+)\/(\d+)$/.exec(p);
    const s = kind === "u" ? utf8(pat(+n, "é" + n + "世")) : pat(+n, (kind === "st" ? "t" : kind) + n + ".");
    return { body: s, cl: s.length };
};

async function check(a, reqs, label) {
    const r = await burst(reqs);
    a.eq(r.got.length, reqs.length, `${label}: every response arrives (${r.why})`);
    let ok = 0;
    r.got.forEach((x, i) => {
        const w = want(reqs[i][0], reqs[i][1]);
        if (x.body === w.body && x.cl === w.cl) ok++;
        else a.ok(false, `${label}: ${reqs[i].join(" ")} got cl=${x.cl} len=${x.body.length} head=${JSON.stringify(x.head.slice(0, 40))}`);
    });
    a.eq(ok, reqs.length, `${label}: bodies byte-exact and in order`);
    return r;
}

const SIZES = [0, 1, 900, 1023, 1024, 1025, 4096, 16383, 16384, 30000, 60000, 65300, 65380, 65400, 65420, 65440, 65536, 65537, 70000, 200000];

t.test("each size alone, string bodies", async (a) => {
    for (const n of SIZES) await check(a, [["GET", "/s/" + n]], "s" + n);
});

t.test("a pipelined burst mixing every size, in request order", async (a) => {
    await check(a, SIZES.map((n) => ["GET", "/s/" + n]), "burst");
    await check(a, SIZES.slice().reverse().map((n) => ["GET", "/s/" + n]), "reverse burst");
});

t.test("UTF-8 and byte bodies across the limits", async (a) => {
    await check(a, SIZES.map((n) => ["GET", "/u/" + n]), "utf8");
    await check(a, SIZES.map((n) => ["GET", "/b/" + n]), "bytes");
});

t.test("a small error answer between large ones keeps its place", async (a) => {
    const reqs = [];
    for (const n of [60000, 65400, 70000]) reqs.push(["GET", "/s/" + n], ["PUT", "/s/" + n], ["GET", "/s/" + (n + 1)]);
    const r = await burst(reqs);
    a.eq(r.got.length, reqs.length, "every response arrives (" + r.why + ")");
    r.got.forEach((x, i) => {
        if (reqs[i][0] === "PUT") a.match(x.head, /^HTTP\/1\.1 405 /, "405 in place " + i);
        else a.eq(x.body, want("GET", reqs[i][1]).body, "body in place " + i);
    });
});

t.test("answers resolved after the dispatch (async handlers) go out whole and in order", async (a) => {
    await check(a, [60000, 1500, 65400, 200, 70000].map((n) => ["GET", "/a/" + n]), "async");
    await check(a, [["GET", "/a/60000"], ["GET", "/s/60001"], ["GET", "/a/1500"], ["GET", "/s/65400"]], "async mixed");
});

t.test("status and extra headers survive on large bodies", async (a) => {
    const reqs = [1500, 60000, 65400, 90000].map((n) => ["GET", "/st/" + n]);
    const r = await check(a, reqs, "status");
    r.got.forEach((x, i) => {
        a.match(x.head, /^HTTP\/1\.1 201 /, "status " + reqs[i][1]);
        a.match(x.head, new RegExp("\\r\\nX-K: v" + reqs[i][1].slice(4) + "(\\r|$)"), "header " + reqs[i][1]);
    });
});

t.test("many connections at once each get their own bytes", async (a) => {
    const all = await Promise.all([0, 1, 2, 3, 4, 5, 6, 7].map((k) => burst([60000 + k, 1500 + k, 65400 - k, 200 + k].map((n) => ["GET", "/s/" + n]))));
    let bad = 0;
    all.forEach((r, k) => r.got.forEach((x, i) => { const n = [60000 + k, 1500 + k, 65400 - k, 200 + k][i]; if (x.body !== pat(n, "s" + n + ".")) bad++; }));
    a.eq(bad, 0);
    a.eq(all.reduce((s, r) => s + r.got.length, 0), 32);
});

await t.run();
app.close();
