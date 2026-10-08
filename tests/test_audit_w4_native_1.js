// flags: --std
// Audit wave 4, batch 1: values coerced without a thrown-and-discarded
// exception, TextDecoder on whole valid input, single-byte counting, and the
// trimmed promise surface. Each case names its ORACLE and its CONTROL; [red]
// marks an assertion that fails on the build before this batch.
import "./httpc.js";
import * as bytes from "dyna:bytes";
import * as enc from "dyna:encoding";
import * as ser from "dyna:serialize";
import * as crypto from "dyna:crypto";
import * as file from "dyna:file";
import * as scrape from "dyna:scrape";
import * as os from "os";
import { Selector, HTMLText, HTMLParse } from "dyna:html";
import { memoryUsage } from "dyna:sys";
import { HTTPClient, HTTPServer } from "dyna:net";
import { suite } from "./kit.js";

const t = suite("test_audit_w4_native_1");
const per = (f, n = 20000) => { let b = Infinity; for (let r = 0; r < 5; r++) { const t0 = performance.now(); for (let i = 0; i < n; i++) f(); b = Math.min(b, (performance.now() - t0) / n * 1e6); } return b; };

// ORACLE: what a byte-taking function returns is a property of the BYTES, not
// of the container they arrive in: a Uint8Array, a DataView and an ArrayBuffer
// over the same 16 bytes give the same answer. The published SHA-256 of sixteen
// 'A' bytes (FIPS 180-4, computed with OpenSSL) anchors one of them.
// SCALING: so the container cannot change the COST either. It did: every
// helper tried one container first by calling an accessor that throws on the
// other, then discarded the exception -- about 1.8 us per call, 15x the work.
// CONTROL: a string input, which never went through that probe.
t.test("byte containers agree and cost the same", ({ ok, eq }) => {
    const u8 = new Uint8Array(16).fill(65), ab = u8.buffer.slice(0), dv = new DataView(u8.buffer.slice(0));
    const td = new TextDecoder();
    const fns = {
        "TextDecoder.decode": (x) => td.decode(x),
        "HexEncode": (x) => enc.HexEncode(x),
        "Base64Encode": (x) => enc.Base64Encode(x),
        "SHA256Hex": (x) => crypto.SHA256Hex(x),
        "bytes.isValidUtf8": (x) => bytes.isValidUtf8(x),
        "bytes.equal": (x) => bytes.equal(x, x),
    };
    eq(crypto.SHA256Hex(u8), "991204fba2b6216d476282d375ab88d20e6108d109aecded97ef424ddd114706", "SHA-256 of sixteen 'A' bytes");
    for (const [name, f] of Object.entries(fns)) {
        const a = f(u8), b = f(ab), c = f(dv);
        ok(a === b && b === c, name + ": Uint8Array, ArrayBuffer and DataView give one answer (" + a + " / " + b + " / " + c + ")");
        const cu = per(() => f(u8)), cb = per(() => f(ab)), cd = per(() => f(dv));
        const lo = Math.min(cu, cb, cd), hi = Math.max(cu, cb, cd);
        ok(hi < lo * 4 + 150, "[red] " + name + ": no container costs several times another (" + cu.toFixed(0) + " / " + cb.toFixed(0) + " / " + cd.toFixed(0) + " ns)");
    }
    eq(enc.HexEncode("AAAAAAAAAAAAAAAA"), enc.HexEncode(u8), "control: the string form agrees");
});

// ORACLE: CBOR (RFC 8949 3.1) and MessagePack encode a map as a header plus
// its pairs; encoding N small maps is N times a constant, and a JSON
// serialisation of the same value does the same walk. So binary encoding
// cannot cost several times JSON.stringify -- it cost 15x, because every plain
// object was first probed as a byte view by an accessor that throws.
// RFC 8949 Appendix A gives the vectors: {"a": 1, "b": [2, 3]} is
// a26161016162820203, [1, [2, 3], [4, 5]] is 8301820203820405.
// CONTROL: a flat array of numbers, which never took the probe.
t.test("encoding plain objects costs what the walk costs", ({ ok, eq, deep, best }) => {
    const hex = (u) => enc.HexEncode(u);
    eq(hex(ser.CBOREncode({ a: 1, b: [2, 3] })), "a26161016162820203", "RFC 8949 Appendix A map");
    eq(hex(ser.CBOREncode([1, [2, 3], [4, 5]])), "8301820203820405", "RFC 8949 Appendix A nested array");
    const objs = new Array(20000).fill(0).map((_, i) => ({ a: i, s: "v" + i }));
    deep(ser.CBORDecode(ser.CBOREncode(objs.slice(0, 50))), objs.slice(0, 50), "CBOR round trip");
    deep(ser.MsgPackDecode(ser.MsgPackEncode(objs.slice(0, 50))), objs.slice(0, 50), "MessagePack round trip");
    deep(ser.structuredClone(objs.slice(0, 50)), objs.slice(0, 50), "structuredClone copies the values");
    const tj = best(() => JSON.stringify(objs)), tc = best(() => ser.CBOREncode(objs)), tm = best(() => ser.MsgPackEncode(objs)), ts = best(() => ser.structuredClone(objs));
    ok(tc < tj * 5, "[red] CBOREncode of 20000 small objects costs about what JSON.stringify does (" + tc.toFixed(2) + " ms vs " + tj.toFixed(2) + " ms)");
    ok(tm < tj * 5, "[red] MsgPackEncode likewise (" + tm.toFixed(2) + " ms)");
    ok(ts < tj * 12, "[red] structuredClone builds a copy in a small multiple of that (" + ts.toFixed(2) + " ms)");
    const flat = new Array(100000).fill(0).map((_, i) => i * 1.5);
    ok(best(() => ser.CBOREncode(flat)) < best(() => JSON.stringify(flat)) * 3, "control: a flat number array was never slow");
});

// ORACLE: the WHATWG Encoding Standard's UTF-8 decoder is a function of the
// byte SEQUENCE: feeding it one byte at a time with {stream: true} yields the
// same text as one call. Valid input now skips the byte-wise state machine
// after one vectorised validation, so the two paths are different code and
// must still agree -- on valid text, on every malformed form (lone
// continuation, overlong, surrogate, beyond U+10FFFF, truncated tail), and
// with those forms placed across the 32-byte gate.
// SCALING: 4 MiB of ASCII decodes at memory speed, not at 430 MB/s.
// CONTROL: an input that is invalid only at its tail takes the old path at
// the old speed and still produces U+FFFD.
t.test("TextDecoder: whole valid input and byte-wise streaming agree", ({ ok, eq, best }) => {
    const whole = (u, fatal) => new TextDecoder("utf-8", { fatal }).decode(u);
    const drip = (u, fatal) => { const d = new TextDecoder("utf-8", { fatal }); let s = ""; for (let i = 0; i < u.length; i++) s += d.decode(u.subarray(i, i + 1), { stream: true }); return s + d.decode(); };
    const pad = (n) => new Array(n).fill(0x61);
    const forms = {
        "ascii": [0x68, 0x69],
        "2-byte": [0xc3, 0xa9], "3-byte": [0xe2, 0x9c, 0x93], "4-byte": [0xf0, 0x9f, 0x98, 0x80],
        "BOM": [0xef, 0xbb, 0xbf],
        "lone continuation": [0x80], "overlong 2": [0xc0, 0xaf], "overlong 3": [0xe0, 0x80, 0xaf],
        "surrogate": [0xed, 0xa0, 0x80], "beyond U+10FFFF": [0xf4, 0x90, 0x80, 0x80], "invalid lead": [0xff],
        "truncated 3-byte": [0xe2, 0x9c], "truncated 4-byte": [0xf0, 0x9f],
    };
    let bad = 0, n = 0;
    for (const [name, f] of Object.entries(forms)) {
        for (const before of [0, 1, 29, 30, 31, 32, 33, 64]) {
            for (const after of [0, 1, 31, 32, 40]) {
                const u = new Uint8Array([...pad(before), ...f, ...pad(after)]);
                n++;
                if (whole(u, false) !== drip(u, false)) { bad++; if (bad < 4) console.log("  differs: " + name + " before=" + before + " after=" + after); }
                let w, d;
                try { w = whole(u, true); } catch (e) { w = "THROW"; }
                try { d = drip(u, true); } catch (e) { d = "THROW"; }
                n++;
                if (w !== d) { bad++; if (bad < 4) console.log("  fatal differs: " + name + " before=" + before + " after=" + after); }
            }
        }
    }
    eq(bad, 0, "whole and byte-wise decoding agree on " + n + " inputs");
    eq(whole(new Uint8Array([...pad(40), 0xe2, 0x9c, 0x93]), false), "a".repeat(40) + "✓", "a 3-byte sequence after 40 ASCII bytes");
    eq(whole(new Uint8Array([0xef, 0xbb, 0xbf, ...pad(40)]), false), "a".repeat(40), "a leading BOM is stripped on the whole-input path");
    eq(whole(new Uint8Array([...pad(40), 0xff]), false), "a".repeat(40) + "�", "control: an invalid tail still becomes U+FFFD");

    const big = new TextEncoder().encode("plain ascii prose with few specials and no surprises at all ".repeat(70000));
    const tailBad = big.slice(); tailBad[tailBad.length - 3] = 0xff;
    const d = new TextDecoder();
    ok(d.decode(big).length === big.length, "4 MiB of ASCII decodes to as many characters");
    const tGood = best(() => d.decode(big)), tBad = best(() => d.decode(tailBad));
    ok(tGood * 4 < tBad, "[red] valid input skips the byte-wise decoder (" + tGood.toFixed(2) + " ms vs " + tBad.toFixed(2) + " ms for the same bytes with an invalid tail)");
});

// ORACLE: count(buf, byte) is the number of positions holding that byte, the
// definition a plain loop computes; checked at every length around the vector
// widths and from every start offset.
// SCALING: a dense byte is counted in one pass, not by restarting a substring
// search after every hit (502 MB/s on text with a space every fifth byte).
// CONTROL: a multi-byte needle, which still takes the search path.
t.test("bytes.count of one byte is one pass", ({ ok, eq, best }) => {
    let bad = 0;
    for (const len of [0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 200]) {
        const u = new Uint8Array(len);
        for (let i = 0; i < len; i++) u[i] = (i * 7) % 5 === 0 ? 0x20 : 0x61;
        for (const start of [0, 1, 16, len]) {
            if (start > len) continue;
            let want = 0;
            for (let i = start; i < len; i++) if (u[i] === 0x20) want++;
            if (bytes.count(u, 0x20, start) !== want) bad++;
        }
    }
    eq(bad, 0, "count agrees with a plain loop at every length and start");
    const text = new TextEncoder().encode("the quick brown fox jumps over the lazy dog ".repeat(90000));
    eq(bytes.count(text, new Uint8Array([0x66, 0x6f, 0x78])), 90000, "control: a three-byte needle is counted by search");
    const tOne = best(() => bytes.count(text, 0x20)), tMiss = best(() => bytes.count(text, 0x7e));
    ok(tOne < tMiss * 6 + 0.2, "[red] a dense byte costs about what an absent one does (" + tOne.toFixed(2) + " ms vs " + tMiss.toFixed(2) + " ms)");
});

// ORACLE: an object that is built and dropped gives its memory back, so the
// allocator's own byte counter is flat from one round of constructions to the
// next. Each Extractor used to keep one reference to every field NAME: 81
// bytes per field, for ever, visible only when the names are distinct.
// The first round is warm-up; the comparison is the second round against the
// third. CONTROL: the extractor still extracts.
t.test("Extractor releases its field names", ({ ok, eq }) => {
    const sel = new Selector("h1");
    const one = new scrape.Extractor({ title: { sel } }, { text: HTMLText });
    eq(one.run(HTMLParse("<h1>Hi</h1>")).value.title, "Hi", "control: a field is extracted");
    const round = (base) => { for (let i = 0; i < 20000; i++) { const spec = {}; spec["field_with_a_long_distinct_name_" + (base + i)] = { sel }; new scrape.Extractor(spec, { text: HTMLText }); } };
    round(0);
    const m1 = memoryUsage().mallocSize;
    round(100000);
    const m2 = memoryUsage().mallocSize;
    ok(m2 - m1 < 200000, "[red] 20000 more extractors retain nothing (" + (m2 - m1) + " bytes grew)");
});

// ORACLE: the reference (dynajs.d.ts) declares one blocking form of each of
// these operations and no promise-returning twin. The binary's own reflection
// is the check, walked through prototypes so non-enumerable members count.
// CONTROL: the blocking forms are present, fetch() is the promise-based
// client, and an exchange with a server in this process completes through it.
t.test("no promise twin of a blocking call is exported", async ({ ok, eq }) => {
    const names = (o) => { const s = new Set(); for (let p = o; p && p !== Object.prototype && p !== Function.prototype; p = Object.getPrototypeOf(p)) for (const k of Object.getOwnPropertyNames(p)) s.add(k); return s; };
    const gone = [
        [file, ["readFileAsync", "writeFileAsync", "copyFileAsync", "asyncStats"]],
        [file.FileWriter.prototype, ["syncAsync"]],
        [crypto.Argon2id, ["hashAsync", "verifyAsync", "asyncStats"]],
        [scrape.Fetcher.prototype, ["getAsync"]],
        [os, ["sleepAsync"]],
    ];
    const left = [];
    for (const [o, list] of gone) { const have = names(o); for (const n of list) if (have.has(n)) left.push(n); }
    eq(left.join(","), "", "[red] removed names still present");
    const own = Object.getOwnPropertyNames(Object.getPrototypeOf(new HTTPClient()));
    ok(!own.includes("getAsync") || Object.getOwnPropertyDescriptor(HTTPClient.prototype, "getAsync").value.toString().includes("PENDING"),
        "[red] HTTPClient's own getAsync is the test shim, not a native method");
    for (const [o, list] of [[file, ["readFile", "writeFile", "copyFile"]], [file.FileWriter.prototype, ["sync"]], [crypto.Argon2id, ["hash", "verify"]], [scrape.Fetcher.prototype, ["get"]], [os, ["sleep"]], [HTTPClient.prototype, ["get", "post", "request"]]])
        for (const n of list) ok(names(o).has(n), "control: " + n + " is present");
    const srv = new HTTPServer({ port: 0, routes: { "/ping": "pong" } });
    srv.start();
    const r = await fetch("http://127.0.0.1:" + srv.port + "/ping");
    eq(r.status + " " + (await r.text()), "200 pong", "control: fetch() completes against a server in this process");
    srv.close();
}, { timeoutMs: 15000 });

await t.run();
