// flags: --std
// timeout: 240
// covers: dyna:ai.Anthropic -- client.files: upload/list/retrieveMetadata/download/delete
// The Files API (GA, no beta header; platform.claude.com/docs/en/build-with-claude/files.md)
// against a raw TCP mock on 127.0.0.1 that keeps every request byte, so the multipart body
// is compared byte for byte (all 256 byte values, a UTF-8 filename, optional part
// Content-Type, the expires_in_seconds field) and repeated ids[] parameters stay visible.
// Client-side checks (filename 1..255 characters without < > : " | ? * \ / or 0-31,
// expiresInSeconds an integer 3600..7776000, ids at most 100) reject before any I/O.
// download() resolves a Uint8Array; the API's 400 for a non-downloadable file is an APIError.
// No live calls: apiKey "test-key", maxRetries 0.
import { sections as __kitSections } from "../lib/kit.js";
import { Anthropic, APIError } from "dyna:ai";
import { TCPServer } from "dyna:net";
import * as std from "std";
const __kitS = __kitSections("test_ai_anthropic_files", { chain: true });
let enc, dec, srv, bufs, ALL, c;

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); } else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const rejects = async (p, pred, w) => {
    try { await p; ok(false, w, "resolved"); }
    catch (e) { ok(pred(e), w, e && (e.constructor.name + ": " + e.message + (e.status !== undefined ? " status=" + e.status + " type=" + e.type : ""))); }
};
const cat = (...parts) => { const n = parts.reduce((a, p) => a + p.length, 0), o = new Uint8Array(n); let i = 0; for (const p of parts) { o.set(p, i); i += p.length; } return o; };
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const find = (h, n, from = 0) => { outer: for (let i = from; i + n.length <= h.length; i++) { for (let j = 0; j < n.length; j++) if (h[i + j] !== n[j]) continue outer; return i; } return -1; };

const log = [];
let route = () => null;
const reply = (status, body, ctype = "application/json", extra = "") => {
    const b = typeof body === "string" ? enc.encode(body) : body;
    return cat(enc.encode("HTTP/1.1 " + status + " X\r\nContent-Type: " + ctype + "\r\nContent-Length: " + b.length + "\r\nrequest-id: req_" + status + "\r\n" + extra + "Connection: close\r\n\r\n"), b);
};
const apiErr = (status, type, message) => reply(status, JSON.stringify({ type: "error", error: { type, message }, request_id: "req_e" }));
const fileObj = (id, name, mime, size) => ({ id, type: "file", filename: name, mime_type: mime, size_bytes: size, created_at: "2026-11-14T00:00:00Z", downloadable: false });
const last = () => log[log.length - 1];
const expectMultipart = (q, parts) => {
    const m = /^multipart\/form-data; boundary=(.+)$/.exec(q.headers["content-type"] || "");
    if (!m) return "no multipart content-type: " + q.headers["content-type"];
    const B = m[1];
    const want = cat(...parts.map((p) => cat(enc.encode("--" + B + "\r\nContent-Disposition: form-data; name=\"" + p.name + "\"" + (p.filename !== undefined ? "; filename=\"" + p.filename + "\"" : "") + "\r\n"
        + (p.type ? "Content-Type: " + p.type + "\r\n" : "") + "\r\n"), typeof p.data === "string" ? enc.encode(p.data) : p.data, enc.encode("\r\n"))), enc.encode("--" + B + "--\r\n"));
    if (!same(q.body, want)) return "body differs (" + q.body.length + " vs " + want.length + ")";
    if (find(ALL, enc.encode(B)) >= 0) return "boundary occurs in the data";
    return true;
};
__kitS.section("fixture: create the text encoder and decoder", () => {
enc = new TextEncoder(), dec = new TextDecoder();
srv = new TCPServer({ port: 0 });
bufs = new Map();
srv.start({
    data: (c, bytes) => {
        const got = cat(bufs.get(c) || new Uint8Array(0), bytes);
        const he = find(got, enc.encode("\r\n\r\n"));
        if (he < 0) { bufs.set(c, got); return; }
        const headText = dec.decode(got.subarray(0, he));
        const cl = /\r\ncontent-length: *(\d+)/i.exec(headText);
        const need = cl ? Number(cl[1]) : 0;
        if (got.length - he - 4 < need) { bufs.set(c, got); return; }
        bufs.delete(c);
        const lines = headText.split("\r\n"), [method, target] = lines[0].split(" ");
        const headers = {};
        for (const l of lines.slice(1)) { const k = l.indexOf(":"); headers[l.slice(0, k).toLowerCase()] = l.slice(k + 1).trim(); }
        const req = { method, target, headers, body: got.slice(he + 4, he + 4 + need) };
        log.push(req);
        c.write(route(req) || apiErr(404, "not_found_error", "no route " + method + " " + target));
        if (c.bufferedAmount === 0) c.close(); else c.closeOnDrain = true;
    },
    drain: (c) => { if (c.closeOnDrain) c.close(); },
    close: (c) => { bufs.delete(c); },
});

ALL = new Uint8Array(1024).map((_, i) => i & 255);
route = (q) => {
    if (q.method === "POST" && q.target === "/v1/files") {
        return reply(200, JSON.stringify(fileObj("file_up", "x", "y", q.body.length)));
    }
    if (q.method === "GET" && q.target.startsWith("/v1/files?") || q.target === "/v1/files")
        return reply(200, JSON.stringify({ data: [], next_page: null }));
    if (q.method === "GET" && q.target === "/v1/files/file_dl/content") return reply(200, ALL, "application/octet-stream");
    if (q.method === "GET" && q.target === "/v1/files/file_empty/content") return reply(200, new Uint8Array(0), "application/octet-stream");
    if (q.method === "GET" && q.target === "/v1/files/file_up/content") return apiErr(400, "invalid_request_error", "File is not downloadable");
    if (q.method === "GET" && q.target === "/v1/files/gone") return apiErr(404, "not_found_error", "File not found");
    if (q.method === "GET" && q.target.startsWith("/v1/files/")) return reply(200, JSON.stringify(fileObj(decodeURIComponent(q.target.slice(10)), "a.txt", "text/plain", 3)));
    if (q.method === "DELETE" && q.target.startsWith("/v1/files/")) return reply(200, JSON.stringify({ id: decodeURIComponent(q.target.slice(10)), type: "file_deleted" }));
    return null;
};

c = new Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:" + srv.port, maxRetries: 0 });
});

try {
    {
let f, q;
await __kitS.section("upload POSTs /v1/files", async () => {
        log.length = 0;
        f = await c.files.upload({ data: ALL, filename: "rëport (1).bin", mimeType: "application/octet-stream" });
        q = last();
        ok(log.length === 1 && q.method === "POST" && q.target === "/v1/files", "upload POSTs /v1/files", q.method + " " + q.target);
        ok(expectMultipart(q, [{ name: "file", filename: "rëport (1).bin", type: "application/octet-stream", data: ALL }]) === true,
            "upload: the multipart body is byte-exact over all 256 byte values with a UTF-8 filename", expectMultipart(q, [{ name: "file", filename: "rëport (1).bin", type: "application/octet-stream", data: ALL }]));
        ok(!("anthropic-beta" in q.headers), "upload sends no anthropic-beta header (Files API is GA)");
        ok(q.headers["x-api-key"] === "test-key" && q.headers["anthropic-version"] === "2026-10-21" && q.headers["accept"] === "application/json", "upload sends the key, version and JSON accept");
        ok(Number(q.headers["content-length"]) === q.body.length, "upload Content-Length matches the body");
        ok(f.id === "file_up" && f.type === "file" && f.size_bytes === q.body.length, "upload resolves the File object", JSON.stringify(f));
});
    }
    {
let r, r2, b1, b2;
await __kitS.section("text/plain; charset=utf-8", async () => {
        await c.files.upload({ data: "héllo \u{1F30D}", filename: "n.txt", mimeType: "text/plain; charset=utf-8", expiresInSeconds: 3600 });
        r = expectMultipart(last(), [{ name: "file", filename: "n.txt", type: "text/plain; charset=utf-8", data: "héllo \u{1F30D}" }, { name: "expires_in_seconds", data: "3600" }]);
        ok(r === true, "upload: a string is sent as UTF-8 and expiresInSeconds as an expires_in_seconds field", r);
});
await __kitS.section("expires_in_seconds", async () => {
        await c.files.upload({ data: ALL.buffer.slice(0, 7), filename: "a".repeat(255), expiresInSeconds: 7776000 });
        r2 = expectMultipart(last(), [{ name: "file", filename: "a".repeat(255), data: ALL.subarray(0, 7) }, { name: "expires_in_seconds", data: "7776000" }]);
        ok(r2 === true, "upload: an ArrayBuffer, no mimeType (no part Content-Type), 255-char filename, 7776000 s", r2);
});
await __kitS.section("upload: a subarray view; 255 two-byte characters is within the limit", async () => {
        await c.files.upload({ data: ALL.subarray(250, 260), filename: "é".repeat(255) });
        ok(expectMultipart(last(), [{ name: "file", filename: "é".repeat(255), data: ALL.subarray(250, 260) }]) === true, "upload: a subarray view; 255 two-byte characters is within the limit");
});
await __kitS.section("each upload picks a fresh RFC 2046 boundary", async () => {
        b1 = /boundary=(.+)$/.exec(last().headers["content-type"])[1];
        await c.files.upload({ data: "x", filename: "x" });
        b2 = /boundary=(.+)$/.exec(last().headers["content-type"])[1];
        ok(b1 !== b2 && /^[0-9A-Za-z-]{1,70}$/.test(b1), "each upload picks a fresh RFC 2046 boundary", b1 + " / " + b2);
});
    }
    {
let n0;
        const te = (e) => e instanceof TypeError, re = (e) => e instanceof RangeError;
await __kitS.section("upload rejects a filename", async () => {
        n0 = log.length;
        for (const [fn, why] of [["", "empty"], ["a/b", "slash"], ["a\\b", "backslash"], ["a:b", "colon"], ["a\"b", "quote"], ["a|b", "pipe"], ["a?b", "question"],
                ["a*b", "star"], ["a<b", "lt"], ["a>b", "gt"], ["a\u0001b", "control 1"], ["a\r\nb", "CRLF"], ["a\u001fb", "control 31"], ["x".repeat(256), "256 chars"], ["é".repeat(256), "256 two-byte chars"]])
            await rejects(c.files.upload({ data: "x", filename: fn }), te, "upload rejects a filename: " + why);
        await rejects(c.files.upload({ data: "x" }), te, "upload rejects a missing filename");
        await rejects(c.files.upload({ data: 5, filename: "a" }), te, "upload rejects numeric data");
        await rejects(c.files.upload({ data: [1, 2], filename: "a" }), te, "upload rejects a plain array");
        await rejects(c.files.upload({ data: "x", filename: "a", mimeType: "a\r\nX-Evil: 1" }), te, "upload rejects a mimeType with CRLF");
        await rejects(c.files.upload({ data: "x", filename: "a", mimeType: "" }), te, "upload rejects an empty mimeType");
        await rejects(c.files.upload({ data: "x", filename: "a", expiresInSeconds: 3599 }), re, "upload rejects expiresInSeconds 3599");
        await rejects(c.files.upload({ data: "x", filename: "a", expiresInSeconds: 7776001 }), re, "upload rejects expiresInSeconds 7776001");
        await rejects(c.files.upload({ data: "x", filename: "a", expiresInSeconds: 3600.5 }), te, "upload rejects a fractional expiresInSeconds");
        await rejects(c.files.upload({ data: "x", filename: "a", expiresInSeconds: "3600" }), te, "upload rejects a string expiresInSeconds");
        await rejects(c.files.upload({ data: "x", filename: "a", purpose: "x" }), (e) => te(e) && /purpose/.test(e.message), "upload rejects an unknown key, naming it");
        await rejects(c.files.upload("x"), te, "upload rejects a non-object argument");
        ok(log.length === n0, "no rejected upload reached the wire", log.length - n0);
});
    }
    {
let n0;
await __kitS.section("list() GETs /v1/files", async () => {
        await c.files.list();
        ok(last().method === "GET" && last().target === "/v1/files", "list() GETs /v1/files", last().target);
});
await __kitS.section("list passes limit and page", async () => {
        await c.files.list({ limit: 5, page: "p 1" });
        ok(last().target === "/v1/files?limit=5&page=p%201", "list passes limit and page", last().target);
});
await __kitS.section("list sends ids as repeated ids[]", async () => {
        await c.files.list({ ids: ["file_a", "f/b"] });
        ok(last().target === "/v1/files?ids%5B%5D=file_a&ids%5B%5D=f%2Fb", "list sends ids as repeated ids[]", last().target);
});
await __kitS.section("list rejects 101 ids", async () => {
        n0 = log.length;
        await rejects(c.files.list({ ids: Array.from({ length: 101 }, (_, i) => "f" + i) }), (e) => e instanceof RangeError, "list rejects 101 ids");
        await rejects(c.files.list({ ids: "file_a" }), (e) => e instanceof RangeError, "list rejects ids that is not an array");
        await rejects(c.files.list({ limit: { a: 1 } }), (e) => e instanceof TypeError, "list rejects an object-valued parameter");
        ok(log.length === n0, "rejected lists never reached the wire");
});
await __kitS.section("list accepts 100 ids", async () => {
        await c.files.list({ ids: Array.from({ length: 100 }, (_, i) => "f" + i) });
        ok(last().target.split("ids%5B%5D=").length === 101, "list accepts 100 ids");
});
    }
    {
let m, n0;
await __kitS.section("retrieveMetadata GETs /v1/files/{id}", async () => {
        m = await c.files.retrieveMetadata("file_1");
        ok(last().method === "GET" && last().target === "/v1/files/file_1" && m.id === "file_1" && m.type === "file", "retrieveMetadata GETs /v1/files/{id}", last().target);
});
await __kitS.section("the id is one percent-encoded path segment", async () => {
        await c.files.retrieveMetadata("a/b c");
        ok(last().target === "/v1/files/a%2Fb%20c", "the id is one percent-encoded path segment", last().target);
        await rejects(c.files.retrieveMetadata("gone"), (e) => e instanceof APIError && e.status === 404 && e.type === "not_found_error" && e.requestId === "req_e", "a 404 is an APIError not_found_error");
});
await __kitS.section("an empty id is a TypeError", async () => {
        n0 = log.length;
        await rejects(c.files.retrieveMetadata(""), (e) => e instanceof TypeError, "an empty id is a TypeError");
        await rejects(c.files.download(42), (e) => e instanceof TypeError, "a numeric id is a TypeError");
        await rejects(c.files.delete(), (e) => e instanceof TypeError, "a missing id is a TypeError");
        ok(log.length === n0, "bad ids never reached the wire");
});
    }
    {
let d, e0;
await __kitS.section("download resolves the content as a Uint8Array, byte-exact", async () => {
        d = await c.files.download("file_dl");
        ok(d instanceof Uint8Array && same(d, ALL), "download resolves the content as a Uint8Array, byte-exact", d && d.length);
        ok(last().target === "/v1/files/file_dl/content" && last().headers["accept"] === "*/*", "download GETs /content with Accept: */*", last().headers["accept"]);
});
await __kitS.section("an empty file downloads as an empty Uint8Array", async () => {
        e0 = await c.files.download("file_empty");
        ok(e0 instanceof Uint8Array && e0.length === 0, "an empty file downloads as an empty Uint8Array");
        await rejects(c.files.download("file_up"), (e) => e instanceof APIError && e.status === 400 && e.type === "invalid_request_error" && /not downloadable/.test(e.message),
            "downloading an uploaded (non-downloadable) file is a 400 APIError");
});
    }
await __kitS.section("delete sends DELETE /v1/files/{id}", async () => {
    {
        const r = await c.files.delete("file_1");
        ok(last().method === "DELETE" && last().target === "/v1/files/file_1" && r.type === "file_deleted", "delete sends DELETE /v1/files/{id}", JSON.stringify(r));
    }
});
    {
let n0;
await __kitS.section("a closed client rejects files calls", async () => {
        n0 = log.length;
        c.close();
        await rejects(c.files.list(), (e) => e instanceof TypeError && /closed/.test(e.message), "a closed client rejects files calls");
        ok(log.length === n0, "a closed client sends nothing");
});
    }
} finally { if (!__kitS.listing) {
    srv.close();
} }
__kitS.end();
if (!__kitS.listing) {
print(`test_ai_anthropic_files: ${pass} passed, ${fail} failed`);
std.exit(fail ? 1 : 0);
}
