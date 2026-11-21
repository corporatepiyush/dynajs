// covers: integration c:src/modules/libc/libc-fetch.c c:src/modules/http/http-client.c -- body handling of the fetch global over the http module client against a raw dyna:net TCPServer peer
// Response.body is a WHATWG byte ReadableStream (Fetch spec "body" getter), and a ReadableStream is accepted as a Response/Request body.
// The fetch JS is evaluated at startup, so it must not touch the lazy Streams globals until a body stream is asked for: the first case
// checks that a plain Response never evaluates the Streams core. Request.body keeps returning the init value as given (pinned by
// test_fetch_body.js), so a stream request body comes back as that same stream.
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";

const t = suite("test_http_fetch_body_stream");
const enc = new TextEncoder();
async function all(rs) { const parts = []; for await (const c of rs) parts.push(...c); return parts; }

t.test("plain bodies never evaluate the Streams core", async (a) => {
    const before = Object.getOwnPropertyDescriptor(globalThis, "ReadableStream");
    a.eq(typeof before.get, "function", "ReadableStream is still the lazy accessor at startup");
    const r = new Response("x");
    a.eq(await r.text(), "x");
    a.eq(typeof Object.getOwnPropertyDescriptor(globalThis, "ReadableStream").get, "function", "still lazy after a whole Response round trip");
});

t.test("body is a byte ReadableStream, the same object each time; null bodies give null", async (a) => {
    const r = new Response("héllo");
    const b = r.body;
    a.ok(b instanceof ReadableStream);
    a.eq(r.body, b, "same object");
    a.eq(r.bodyUsed, false, "accessing body does not use it");
    const br = b.getReader({ mode: "byob" });
    const { value } = await br.read(new Uint8Array(64));
    a.deep(Array.from(value), Array.from(enc.encode("héllo")), "a byte stream: BYOB reads work");
    a.eq(r.bodyUsed, true, "reading the stream uses the body");
    a.eq(new Response(null).body, null);
    a.eq(new Response().body, null);
    a.deep(await all(new Response(new Uint8Array([1, 2, 3])).body), [1, 2, 3]);
    a.deep(await all(new Response(new Uint8Array([4, 5]).buffer).body), [4, 5]);
});

t.test("body methods read through an existing body stream: locked or disturbed is a TypeError", async (a) => {
    const r = new Response("abc");
    const rd = r.body.getReader();
    a.throws(() => r.clone(), TypeError, "clone while locked");
    await a.rejects(r.text(), TypeError, "text while locked");
    a.eq(r.bodyUsed, false, "a refused text() leaves the body unused");
    await rd.read();
    rd.releaseLock();
    await a.rejects(r.text(), TypeError, "text after the stream was read");
    const r2 = new Response("abc");
    void r2.body;
    a.eq(await r2.text(), "abc", "an untouched body stream is read by text()");
    a.eq(r2.bodyUsed, true);
    const r3 = new Response("abc");
    await r3.body.cancel();
    a.eq(r3.bodyUsed, true, "cancel disturbs");
});

t.test("after text(), body is a disturbed empty stream", async (a) => {
    const r = new Response("abc");
    await r.text();
    const b = r.body;
    a.ok(b instanceof ReadableStream);
    a.eq(r.bodyUsed, true);
    a.eq((await b.getReader().read()).done, true);
});

t.test("a ReadableStream body: text/bytes/arrayBuffer/json, chunk type checked, locked or disturbed refused", async (a) => {
    const mk = (...xs) => new Response(ReadableStream.from(xs.map((x) => enc.encode(x))));
    a.eq(await mk("he", "llo").text(), "hello");
    a.deep(Array.from(await mk("ab").bytes()), [97, 98]);
    a.eq((await mk("abc").arrayBuffer()).byteLength, 3);
    a.deep(await mk('{"k":', "[1]}").json(), { k: [1] });
    await a.rejects(new Response(ReadableStream.from(["not bytes"])).text(), TypeError, "string chunks are refused");
    const s = ReadableStream.from([enc.encode("z")]);
    s.getReader();
    a.throws(() => new Response(s), TypeError, "locked stream");
    const d = new ReadableStream({ type: "bytes" });
    await d.cancel();
    a.throws(() => new Response(d), TypeError, "disturbed stream");
    const r = mk("q");
    a.eq(r.body instanceof ReadableStream, true);
    a.eq(await r.text(), "q");
    await a.rejects(r.text(), TypeError, "second read");
});

t.test("clone() tees a stream body: both read the same bytes", async (a) => {
    const r = new Response(ReadableStream.from([enc.encode("one"), enc.encode("two")]));
    const c = r.clone();
    a.eq(await r.text(), "onetwo");
    a.eq(await c.text(), "onetwo");
    const r2 = new Response("plain");
    void r2.body;
    const c2 = r2.clone();
    a.eq(await c2.text(), "plain");
    a.eq(await r2.text(), "plain");
});

t.test("Request: a stream body is read by text()/bytes(), body returns the stream as given", async (a) => {
    const s = ReadableStream.from([enc.encode("rq")]);
    let threw = null;
    try { new Request("http://h/", { method: "POST", body: ReadableStream.from([]) }); } catch (e) { threw = e; }
    a.ok(threw instanceof TypeError, "Fetch spec: a ReadableStream body without duplex 'half' is a TypeError");
    const q = new Request("http://h/", { method: "POST", body: s, duplex: "half" });
    a.eq(q.body, s);
    a.eq(await q.text(), "rq");
});

t.test("fetch: a stream request body is sent whole and byte-exact; the response body streams", async (a) => {
    const srv = new TCPServer({ port: 0 });
    let got = "";
    const raw = [];
    srv.start({
        data: (c, bytes) => {
            raw.push(...bytes);
            got += new TextDecoder().decode(bytes);
            if (got.indexOf("\r\n\r\n") >= 0 && raw.length >= got.indexOf("\r\n\r\n") + 4 + 7) c.write("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world");
        },
    });
    const r = await fetch("http://127.0.0.1:" + srv.port + "/u", { method: "POST", body: ReadableStream.from([enc.encode("ping-"), new Uint8Array([0xff, 0xfe])]), duplex: "half" });
    a.ok(/Content-Length: 7/i.test(got), "buffered upload has a Content-Length of the raw bytes");
    a.deep(raw.slice(-7), [0x70, 0x69, 0x6e, 0x67, 0x2d, 0xff, 0xfe], "non-UTF-8 bytes go out unchanged");
    const parts = [];
    for await (const c of r.body.pipeThrough(new TextDecoderStream())) parts.push(c);
    a.eq(parts.join(""), "hello world");
    a.eq(r.bodyUsed, true);
    srv.close();
});

await t.run();
