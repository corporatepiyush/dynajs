// flags: --std
import { App, TCPServer } from "dyna:net";
import { gunzip } from "dyna:compress";
import * as std from "std";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");

function raw(port, headers, path, body) {
    let buf = "", conn = null, cli;
    const connected = new Promise((res) => {
        cli = TCPServer.connect({ host: "127.0.0.1", port }, {
            connect: (c, err) => { conn = c; res(); },
            data: (c, b) => {
                for (let i = 0; i < b.length; i++)
                    buf += String.fromCharCode(b[i]);
            },
            close: () => {},
        });
    });
    return {
        client: cli,
        async ask() {
            await connected;
            const b = body !== undefined ? body
                : JSON.stringify({ jsonrpc: "2.0", method: "big", params: [], id: 1 });
            conn.write("POST " + (path || "/big") + " HTTP/1.1\r\nHost: x\r\n"
                       + headers.map(([k, v]) => k + ": " + v + "\r\n").join("")
                       + "Content-Length: " + b.length + "\r\n\r\n" + b);
            await new Promise((res) => setTimeout(res, 350));
            return buf;
        },
        close() { try { conn ? conn.close() : null; } catch (e) {} },
    };
}
const BODY = "The quick brown fox jumps over the lazy dog. ".repeat(40);

function makeApp(compress) {
    const app = new App({ port: 0, idleTimeoutMs: 5000, compress });
    app.rpc("/big", { big: () => BODY });
    app.start();
    return app;
}
const expectBody = '{"jsonrpc":"2.0","result":"' + BODY + '","id":1}';

function parse(resp) {
    const i = resp.indexOf("\r\n\r\n");
    const head = i >= 0 ? resp.slice(0, i) : resp;
    const body = i >= 0 ? resp.slice(i + 4) : "";
    const hdr = (n) => {
        const m = head.match(new RegExp("^" + n + ": (.+)$", "mi"));
        return m ? m[1].trim() : null;
    };
    return { head, body, hdr };
}
const bytesOf = (s) => new Uint8Array([...s].map((ch) => ch.charCodeAt(0)));
const dec = (u8) => new TextDecoder().decode(u8);

{
    const app = makeApp(true);
    const cases = [
        ["gzip", "gzip"],
        ["*", "gzip"],
        ["gzip, deflate", "gzip"],
        ["br, gzip", "gzip"],
        ["GZIP", "gzip"],
        [" xgzip", "identity"],
        ["identity", "identity"],
        [null, "identity"],
        ["gzip;q=0", "identity"],
        ["gzip;q=0.0", "identity"],
        ["gzip;q=0.05", "gzip"],
        ["gzip;q=0.5", "gzip"],
        ["gzip;q=1", "gzip"],
        ["gzip ; q = 0", "identity"],
        ["gzip;level=5;q=0.3", "gzip"],
        ["gzip;q", "gzip"],
        ["gzip;q=0, *;q=1", "identity"],
        ["*;q=0", "406"],
        ["identity;q=0", "406"],
        ["identity;q=0, gzip;q=0", "406"],
        ["identity;q=0, gzip", "gzip"],
    ];
    for (const [accept, want] of cases) {
        const rc = raw(app.port, accept ? [["Accept-Encoding", accept]] : []);
        const resp = await rc.ask();
        const p = parse(resp);
        const label = accept === null ? "(no header)" : JSON.stringify(accept);
        if (want === "406") {
            ok(p.head.indexOf("HTTP/1.1 406") >= 0,
               label + ": refusing every representation is a 406 (" + p.head.split("\r\n")[0] + ")");
        } else if (want === "gzip") {
            ok(p.hdr("Content-Encoding") === "gzip",
               label + ": Content-Encoding is gzip (" + p.head + ")");
            ok(p.hdr("Vary") !== null && p.hdr("Vary").toLowerCase().indexOf("accept-encoding") >= 0,
               label + ": Vary advertises the negotiation");
            const clen = parseInt(p.hdr("Content-Length"), 10);
            ok(clen === p.body.length,
               label + ": Content-Length counts the COMPRESSED body (" + clen + " vs " + p.body.length + ")");
            ok(p.body.length < expectBody.length,
               label + ": the wire body actually shrank");
            ok(dec(gunzip(bytesOf(p.body))) === expectBody,
               label + ": an independent inflate recovers the exact body");
        } else {
            ok(p.hdr("Content-Encoding") === null,
               label + ": no Content-Encoding (" + p.hdr("Content-Encoding") + ")");
            ok(p.body === expectBody,
               label + ": the body is identity, byte for byte");
        }
        rc.close();
    }
    app.close();
}

{
    const app = makeApp(false);
    const rc = raw(app.port, [["Accept-Encoding", "gzip"]]);
    const p = parse(await rc.ask());
    ok(p.hdr("Content-Encoding") === null, "compress:false sends identity");
    ok(p.body === expectBody, "and the raw bytes");
    rc.close();
    app.close();
}

{
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/tiny", { tiny: () => "small" });
    app.start();
    let buf = "", conn = null, cli;
    const connected = new Promise((res) => {
        cli = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (c) => { conn = c; res(); },
            data: (c, b) => { buf += new TextDecoder().decode(b); },
            close: () => {},
        });
    });
    cli;
    await connected;
    conn.write("POST /tiny HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n"
               + "Content-Length: 55\r\n\r\n"
               + '{"jsonrpc":"2.0","method":"tiny","params":[],"id":1}');
    await new Promise((res) => setTimeout(res, 350));
    const p = parse(buf);
    ok(p.hdr("Content-Encoding") === null,
       "a body under the threshold stays identity even when asked");
    try { conn.close(); } catch (e) {}
    app.close();
}

{
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/pipe", {
        big: () => new Promise((res) => setTimeout(() => res(BODY), 250)),
    });
    app.start();
    let buf = "", conn = null, cli;
    const connected = new Promise((res) => {
        cli = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (c) => { conn = c; res(); },
            data: (c, b) => {
                for (let i = 0; i < b.length; i++)
                    buf += String.fromCharCode(b[i]);
            },
            close: () => {},
        });
    });
    cli;
    await connected;
    const body = JSON.stringify({ jsonrpc: "2.0", method: "big", params: [], id: 1 });
    const body2 = JSON.stringify({ jsonrpc: "2.0", method: "big", params: [], id: 2 });
    const req = (id, ae, b) =>
        "POST /pipe HTTP/1.1\r\nHost: x\r\n" + ae +
        "Content-Length: " + b.length + "\r\n\r\n" + b;
    conn.write(req(1, "Accept-Encoding: gzip\r\n", body)
               + req(2, "", body2));
    const deadline = Date.now() + 8000;
    let heads = 0;
    while (Date.now() < deadline) {
        heads = buf.split("HTTP/1.1").length - 1;
        if (heads >= 2) break;
        await new Promise((res) => setTimeout(res, 30));
    }
    eq(heads, 2, "both pipelined responses arrived");
    const i1 = buf.indexOf("\r\n\r\n");
    const clMatch = buf.slice(0, i1).match(/Content-Length: (\d+)/i);
    if (clMatch) {
        const cl1 = parseInt(clMatch[1], 10);
        const r1 = buf.slice(0, i1 + 4 + cl1);
        const r2 = buf.slice(i1 + 4 + cl1);
        ok(r1.indexOf("Content-Encoding: gzip") >= 0,
           "the FIRST response honours ITS OWN Accept-Encoding: gzip");
        ok(r2.indexOf("Content-Encoding: gzip") < 0,
           "the SECOND response honours ITS OWN (none sent): identity");
        const j1 = r1.indexOf("\r\n\r\n");
        const b1 = r1.slice(j1 + 4);
        ok(dec(gunzip(bytesOf(b1))).indexOf('"id":1') >= 0,
           "the compressed response decompresses to the right result");
        const j2 = r2.indexOf("\r\n\r\n");
        ok(r2.slice(j2 + 4).indexOf('"id":2') >= 0,
           "the identity response is the right result");
    }
    try { conn.close(); } catch (e) {}
    app.close();
}

{
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/tiny", { tiny: () => "small" });
    app.start();
    const rc = raw(app.port, [["Accept-Encoding", "identity;q=0, gzip"]], "/tiny",
                   JSON.stringify({ jsonrpc: "2.0", method: "tiny", params: [], id: 1 }));
    const p = parse(await rc.ask());
    ok(p.hdr("Content-Encoding") === "gzip",
       "a body under the threshold is still gzipped when identity is refused");
    ok(dec(gunzip(bytesOf(p.body))).indexOf('"small"') >= 0,
       "and it inflates to the right result");
    rc.close();
    app.close();
}

print("test_http_compress: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
