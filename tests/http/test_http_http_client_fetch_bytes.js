// flags: --std
// timeout: 60
// covers: dyna:http.HTTPClient
// fetch() reads the body as bytes only. The client hook (Symbol.for("dyna.http.request")) takes a 5th
// argument, bytesOnly: fetch passes true and gets {bodyBytes} without the UTF-8-decoded `body` string
// it never used (at 1 MiB that decode was most of the client CPU); every other caller (dyna:scrape)
// still gets both. A fetched Response owns its ArrayBuffer, so bytes()/arrayBuffer() hand it out
// without the extra copy; a Response built by user code over the user's own Uint8Array/ArrayBuffer
// still copies, so mutating either side never shows through the other.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_client_fetch_bytes");
import * as std from "std";
import { App, HTTPClient } from "dyna:http";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

const bin = new Uint8Array(70000);
for (let i = 0; i < bin.length; i++) bin[i] = (i * 131 + 7) & 255;
bin[0] = 0xff; bin[1] = 0xfe; bin[2] = 0xc3; bin[3] = 0x28; bin[4] = 0xed; bin[5] = 0xa0; bin[6] = 0x80;
const sameBytes = (a, b) => { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; };

const app = new App({ port: 0, compress: false });
app.get("/bin", () => ({ body: bin, contentType: "application/octet-stream" }));
app.get("/txt", () => "héllo wörld");
app.get("/empty", () => "");
app.start();
const base = "http://127.0.0.1:" + app.port;

await __kitS.section("fetch bytes(): 70000 bytes incl. invalid UTF-8 arrive intact", async () => {
  const r = await fetch(base + "/bin");
  const u = await r.bytes();
  ok(u instanceof Uint8Array && sameBytes(u, bin), "fetch bytes(): 70000 bytes incl. invalid UTF-8 arrive intact (" + u.length + ")");
  ok(u.byteOffset === 0 && u.byteLength === u.buffer.byteLength, "fetch bytes(): a view over the whole owned buffer");
  ok(r.bodyUsed, "bodyUsed after bytes()");
  let threw = false;
  try { await r.bytes(); } catch (e) { threw = e instanceof TypeError; }
  ok(threw, "second read throws TypeError");
});
await __kitS.section("fetch arrayBuffer(): intact", async () => {
  const r = await fetch(base + "/bin");
  const ab = await r.arrayBuffer();
  ok(ab instanceof ArrayBuffer && ab.byteLength === bin.length && sameBytes(new Uint8Array(ab), bin), "fetch arrayBuffer(): intact");
});
await __kitS.section("héllo wörld", async () => {
  const r = await fetch(base + "/txt");
  ok(await r.text() === "héllo wörld", "fetch text(): UTF-8 decoded from the bytes");
  const e = await fetch(base + "/empty");
  const eb = await e.bytes();
  ok(eb.length === 0, "fetch of an empty body: 0 bytes");
});
await __kitS.section("clone() of a fetched response has its own copy (mutating one read leaves the clone intact)", async () => {
  const r = await fetch(base + "/bin");
  const c = r.clone();
  const a = await r.bytes();
  a[10] ^= 0xff;
  const b = await c.bytes();
  ok(sameBytes(b, bin), "clone() of a fetched response has its own copy (mutating one read leaves the clone intact)");
  ok(r.body instanceof ReadableStream && r.bodyUsed && c.bodyUsed, "after bytes(), Response.body is a ReadableStream and the response stays used");
});
await __kitS.section("user Response over a Uint8Array: bytes() is a copy (caller's array untouched)", async () => {
  const mine = new Uint8Array([1, 2, 3, 4]);
  const r = new Response(mine);
  const got = await r.bytes();
  got[0] = 99;
  ok(mine[0] === 1, "user Response over a Uint8Array: bytes() is a copy (caller's array untouched)");
  const ab = new Uint8Array([5, 6, 7]).buffer;
  const r2 = new Response(ab);
  const g2 = await r2.bytes();
  g2[0] = 99;
  ok(new Uint8Array(ab)[0] === 5, "user Response over an ArrayBuffer: bytes() is a copy");
  const r3 = new Response(ab);
  const a3 = await r3.arrayBuffer();
  ok(a3 !== ab && new Uint8Array(a3)[0] === 5, "user Response over an ArrayBuffer: arrayBuffer() is a copy");
});
await __kitS.section("hook with bytesOnly: no decoded body string, bodyBytes intact", async () => {
  const hook = Symbol.for("dyna.http.request");
  const c = new HTTPClient();
  const lean = await c[hook]("GET", base + "/bin", undefined, undefined, true);
  ok(lean.body === undefined && lean.bodyBytes instanceof ArrayBuffer && sameBytes(new Uint8Array(lean.bodyBytes), bin), "hook with bytesOnly: no decoded body string, bodyBytes intact");
  const full = await c[hook]("GET", base + "/txt", undefined, undefined);
  ok(full.body === "héllo wörld" && full.bodyBytes instanceof ArrayBuffer, "hook without bytesOnly (dyna:scrape's call): body string and bodyBytes both present");
  c.close();
});
__kitS.end();

app.close();
print(`test_fetch_bytes: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
