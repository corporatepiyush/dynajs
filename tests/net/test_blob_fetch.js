// covers: c:src/modules/libc/libc-fetch.c
// Blob/File in fetch and FormData (libc-blob.c hooks called from libc-fetch.c): a Blob body is sent as its
// bytes and sets Content-Type from blob.type when the caller did not; Response/Request.blob() wrap the body
// bytes with the Content-Type header as type; FormData.append/set store a Blob as a File (name "blob", the
// File's own name, or the filename argument) and send it as a file part with its type; non-Blob, non-byte
// values become USVStrings (undefined -> "undefined"). The wire checks use a local TCPServer on port 0.
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";

const t = suite("test_blob_fetch");

function captureServer() {
    const srv = new TCPServer({ port: 0 });
    let wire = "";
    srv.start({
        data: (c, bytes) => {
            wire += new TextDecoder().decode(bytes);
            const end = wire.indexOf("\r\n\r\n");
            const m = /content-length: *(\d+)/i.exec(wire);
            if (end >= 0 && (!m || wire.length - end - 4 >= +m[1]))
                c.write("HTTP/1.1 200 OK\r\nContent-Type: text/x-echo\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
        },
    });
    return { srv, get: () => wire };
}

t.test("Response and Request accept Blob bodies and expose blob()", async ({ eq }) => {
    const b = new Blob(["hi there"], { type: "Text/X-Test" });
    const r = new Response(b);
    eq(r.headers.get("content-type"), "text/x-test", "Content-Type from blob.type");
    eq(new Response(b, { headers: { "content-type": "a/b" } }).headers.get("content-type"), "a/b", "explicit header wins");
    eq(new Response(new Blob(["x"])).headers.has("content-type"), false, "empty type sets nothing");
    const rb = await r.blob();
    eq([rb instanceof Blob, rb.size, rb.type, await rb.text()].join("|"), "true|8|text/x-test|hi there", "blob()");
    eq(await new Response(b).text(), "hi there", "text()");
    eq((await new Response(b).arrayBuffer()).byteLength, 8, "arrayBuffer()");
    const rd = new Response(b).body.getReader();
    eq((await rd.read()).value.length, 8, "body stream");
    const q = new Request("http://127.0.0.1/", { method: "POST", body: b });
    eq(q.headers.get("content-type"), "text/x-test", "Request Content-Type");
    eq((await q.blob()).type, "text/x-test", "Request.blob()");
    eq(await (await new Response("streamed").blob()).text(), "streamed", "string body as blob");
    const used = new Response("x");
    await used.text();
    let rej = null;
    await used.blob().then(() => { rej = "fulfilled"; }, (e) => { rej = e; });
    eq(rej instanceof TypeError, true, "blob() on a used body rejects with a TypeError");
    rej = null;
    const err = new Response(new ReadableStream({ pull(c) { c.error(new RangeError("boom")); } }));
    await err.blob().then(() => { rej = "fulfilled"; }, (e) => { rej = e; });
    eq(rej instanceof RangeError, true, "blob() rejects with the stream's error");
});

t.test("FormData stores Blobs as Files and stringifies other values", ({ eq }) => {
    const fd = new FormData();
    const b = new Blob(["z"], { type: "a/b" });
    fd.append("a", b);
    fd.append("b", b, "n.txt");
    fd.append("c", new File(["q"], "own.bin"));
    fd.append("d", undefined);
    fd.set("e", null);
    fd.append("f", new Uint8Array([1]));
    const got = [...fd].map(([k, v]) => k + "=" + (v instanceof File ? "File:" + v.name + ":" + v.type : typeof v + ":" + String(v)));
    eq(got.join(" "), "a=File:blob:a/b b=File:n.txt:a/b c=File:own.bin: d=string:undefined e=string:null f=object:1", "entries");
    eq(fd.get("a") === fd.get("a"), true, "same File object on reads");
});

t.test("fetch sends a Blob body and a FormData File part on the wire", async ({ eq, ok }) => {
    const cap = captureServer();
    const r = await fetch("http://127.0.0.1:" + cap.srv.port + "/b", { method: "POST", body: new Blob(["blob-body"], { type: "text/x-b" }) });
    const resBlob = await r.blob();
    eq(resBlob.type, "text/x-echo", "response blob type from the wire");
    const w = cap.get();
    ok(/\r\ncontent-type: text\/x-b\r\n/i.test(w), "Content-Type header sent: " + JSON.stringify(w.slice(0, 200)));
    ok(w.endsWith("\r\n\r\nblob-body"), "body bytes sent");
    cap.srv.close();
    const cap2 = captureServer();
    const fd = new FormData();
    fd.append("up", new Blob(["FILEDATA"], { type: "image/x-t" }), "pic.t");
    fd.append("plain", "v");
    await (await fetch("http://127.0.0.1:" + cap2.srv.port + "/f", { method: "POST", body: fd })).text();
    const w2 = cap2.get();
    ok(/Content-Disposition: form-data; name="up"; filename="pic\.t"\r\nContent-Type: image\/x-t\r\n\r\nFILEDATA/i.test(w2), "file part: " + JSON.stringify(w2.slice(-400)));
    ok(/name="plain"\r\n\r\nv\r\n/.test(w2), "plain part");
    cap2.srv.close();
});

await t.run();
