// covers: c:src/modules/libc/libc-fetch.c
// Request/Response.formData() against Fetch "Body mixin" formData(): the body's MIME type essence picks the parser.
// application/x-www-form-urlencoded runs the URL Standard urlencoded parser on the raw bytes: bytes are UTF-8 decoded
// whatever the charset parameter says, '+' is a space, bad bytes become U+FFFD, and a leading '?' is NOT stripped
// (node strips it, a deviation; bun and deno agree with the spec). multipart/form-data runs the multipart parser:
// a part without filename is UTF-8 decoded without BOM (a leading U+FEFF stays, as node does; bun and deno drop it),
// a part with filename becomes a File whose type is the part's Content-Type, or text/plain when absent (node and the
// spec; deno uses application/octet-stream, bun text/plain;charset=utf-8). Any other or missing MIME type, a missing
// boundary, a malformed body or a used body rejects with a TypeError.
import { suite } from "../lib/kit.js";

const t = suite("test_fetch_form_data");
const U = "application/x-www-form-urlencoded";
const show = (fd) => [...fd].map(([k, v]) => JSON.stringify(k) + "=" + (v instanceof File ? `File(${v.name}|${v.type}|${v.size})` : JSON.stringify(v))).join(" ");
async function rejects(p) {
    try { await p; return "fulfilled"; } catch (e) { return e.constructor.name; }
}

t.test("urlencoded: UTF-8 regardless of charset, leading '?' kept, empty pieces skipped", async ({ eq }) => {
    const fd = await new Response("?a=1&b=%C3%A9&c&&=x", { headers: { "content-type": U + ";charset=windows-1252" } }).formData();
    eq(fd instanceof FormData, true, "FormData instance");
    eq(show(fd), "\"?a\"=\"1\" \"b\"=\"é\" \"c\"=\"\" \"\"=\"x\"", "entries");
    const raw = new Uint8Array([0x61, 0x3d, 0xff, 0x26, 0x62, 0x3d, 0x25, 0x46, 0x46, 0x2b, 0x31, 0x26, 0x63, 0x3d, 0xe2, 0x82, 0xac]);
    eq(show(await new Response(raw, { headers: { "content-type": U } }).formData()), "\"a\"=\"�\" \"b\"=\"� 1\" \"c\"=\"€\"", "bad bytes and '+'");
    eq(show(await new Response("a=1", { headers: { "content-type": " Application/X-WWW-Form-Urlencoded ; charset=x" } }).formData()), "\"a\"=\"1\"", "essence is case-insensitive and trimmed");
    eq(show(await new Response("", { headers: { "content-type": U } }).formData()), "", "empty body");
    eq(show(await new Request("http://x/", { method: "POST", body: new URLSearchParams("a=1&b=2") }).formData()), "\"a\"=\"1\" \"b\"=\"2\"", "Request with URLSearchParams body");
});

t.test("multipart: fields keep a BOM, files get text/plain by default and their own type otherwise", async ({ eq }) => {
    const mp = "--B\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\n﻿val\r\n--B\r\nContent-Disposition: form-data; name=\"up\"; filename=\"a.txt\"\r\n\r\nFILE\r\n--B\r\nContent-Disposition: form-data; name=\"t\"; filename=\"b.bin\"\r\nContent-Type: Image/X-T\r\n\r\nXY\r\n--B--\r\n";
    const fd = await new Response(mp, { headers: { "content-type": "multipart/form-data; boundary=B" } }).formData();
    eq(show(fd), "\"f\"=\"﻿val\" \"up\"=File(a.txt|text/plain|4) \"t\"=File(b.bin|image/x-t|2)", "entries");
    eq(await fd.get("up").text(), "FILE", "file contents");
    const src = new FormData();
    src.append("s", "1");
    src.append("f", new Blob(["zz"], { type: "a/b" }), "n.txt");
    src.append("g", new Blob(["q"]));
    eq(show(await new Response(src).formData()), "\"s\"=\"1\" \"f\"=File(n.txt|a/b|2) \"g\"=File(blob|application/octet-stream|1)", "FormData round trip");
    eq(show(await new Request("http://x/", { method: "POST", body: src }).formData()), "\"s\"=\"1\" \"f\"=File(n.txt|a/b|2) \"g\"=File(blob|application/octet-stream|1)", "Request round trip");
});

t.test("rejections: wrong or missing MIME type, bad multipart, used body", async ({ eq }) => {
    eq(await rejects(new Response(new Uint8Array([0x61])).formData()), "TypeError", "no Content-Type");
    eq(await rejects(new Response("a=1").formData()), "TypeError", "text/plain");
    eq(await rejects(new Response("--B--\r\n", { headers: { "content-type": "multipart/form-data" } }).formData()), "TypeError", "no boundary");
    eq(await rejects(new Response("garbage", { headers: { "content-type": "multipart/form-data; boundary=B" } }).formData()), "TypeError", "malformed multipart");
    const used = new Response("a=1", { headers: { "content-type": U } });
    await used.text();
    eq(await rejects(used.formData()), "TypeError", "used body");
    const st = new Response(new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode("k=v&w")); c.close(); } }), { headers: { "content-type": U } });
    eq(show(await st.formData()), "\"k\"=\"v\" \"w\"=\"\"", "stream body");
});

await t.run();
