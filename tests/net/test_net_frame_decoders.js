// timeout: 120
// covers: integration dyna:net.LengthFieldDecoder dyna:net.DelimiterDecoder dyna:net.FixedLengthDecoder -- C5 binds the C4 frame decoders (src/core/dyn-frame.c) to TCPConn: decoder option and conn.setDecoder, the frame and error handlers
// slots: 2
// Owner rule (11-26): defaults are full Netty 4.1 semantics (errors are reported and decoding goes on, failFast, a thrown
// error leaves the rest of the read for the next read or end of input, LineBasedFrameDecoder for {"\r\n","\n"}); the
// dyna-specific behaviours are opt-in through the second "meta" argument: stopOnError (error -> close), rejectUnsafeLength,
// resumeAfterError (owner 11-26: decode the rest of the read right after an error instead of Netty's wait for the next read),
// holdOverlapping (resolve overlapping delimiter sets as on the coalesced stream, end of input resolves a held frame). Exact
// read-boundary cases live in tests/net/test_frame_decoder.c (loopback cannot pin read boundaries); this suite checks the
// binding with boundary-independent inputs.
// C5 (NET-FRAME-DECODERS). A decoder turns a connection's byte stream into frames: one fresh Uint8Array per frame through the
// `frame` handler, never one per TCP segment, and `data` stops firing. The writer here cuts every stream into random small writes
// with event-loop turns between them, so frames straddle reads; the frames must still equal a JS reference framing of the same
// bytes. Also covered: TLS (decodes plaintext), an oversize frame -> `error` and decoding goes on (Netty; Netty's messages), with
// stopOnError -> `error` then `close` with no partial frame, pause() inside a frame handler (frames already decoded wait, none is
// lost), end of input resolving a frame held by holdOverlapping, width-8 lengths (Netty's signed long vs rejectUnsafeLength), the
// LineBasedFrameDecoder path, a decoder set while bytes
// wait in the kernel, and the TypeErrors that keep one owner per connection (setDecoder twice, streams() with a decoder).
// TLS has its own read loop and its own end of stream: a peer's close_notify (openssl s_client at stdin EOF) must resolve a
// held frame exactly like a TCP FIN, and resume() on TLS must deliver frames decoded before the pause first. Under stopOnError the
// error handler fires once even when it calls pause()/resume() (resume re-enters the frame pump). TCPConn has no setNoDelay:
// segmentation comes from many small writes with event-loop turns between them (loopback delivers each as its own read).
import { suite } from "../lib/kit.js";
import { TCPServer, LengthFieldDecoder, DelimiterDecoder, FixedLengthDecoder } from "dyna:net";
import * as crypto from "dyna:crypto";
import { makeTempDir, writeFile, readFile, Path, removeAll } from "dyna:file";
import { Which } from "dyna:sys";
import { spawnHelper } from "../lib/helper.js";

const t = suite("test_net_frame_decoders");
const URING = await import("dyna:uring").then(() => true, () => false);
const T = { timeoutMs: 20000 };
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const enc = (s) => new TextEncoder().encode(s);
const dec = (u) => new TextDecoder().decode(u);

let seed = 0x2545F491;
const rnd = (n) => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return ((seed >>> 0) % n); };

function concat(parts) {
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}

async function writeSegmented(conn, bytes, maxPiece = 17) {
    let o = 0, k = 0;
    while (o < bytes.length) {
        const n = 1 + rnd(maxPiece);
        try { conn.write(bytes.subarray(o, Math.min(bytes.length, o + n))); } catch (e) { return false; }
        o += n;
        if (++k % (1 + rnd(4)) === 0) await tick(rnd(3));
    }
    return true;
}

function lengthFrames(count) {
    const bodies = [], wire = [];
    for (let i = 0; i < count; i++) {
        const n = rnd(i % 7 === 0 ? 3000 : 60);
        const b = new Uint8Array(n);
        for (let j = 0; j < n; j++) b[j] = rnd(256);
        bodies.push(b);
        wire.push(Uint8Array.of(n >> 8, n & 255), b);
    }
    return { bodies, wire: concat(wire) };
}

function sameFrames(a, got, want, what) {
    a.eq(got.length, want.length, what + ": frame count");
    let bad = -1;
    for (let i = 0; i < Math.min(got.length, want.length) && bad < 0; i++) {
        if (got[i].length !== want[i].length) bad = i;
        else for (let j = 0; j < got[i].length; j++) if (got[i][j] !== want[i][j]) { bad = i; break; }
    }
    a.eq(bad, -1, what + ": first differing frame");
}

async function until(fn, ms = 5000) {
    for (let i = 0; i < ms / 5 && !fn(); i++) await tick(5);
    return fn();
}

function serverWith(opts, handlers) {
    const srv = new TCPServer({ port: 0, ...opts });
    srv.start(handlers);
    return srv;
}

function connect(opts, handlers) {
    return new Promise((resolve, reject) => {
        TCPServer.connect(opts, {
            ...handlers,
            connect(c, err) { if (err) { reject(new Error(err)); return; } if (handlers.connect) handlers.connect(c); resolve(c); },
        });
    });
}

t.test("factories: strict bags, defaults, ranges, not constructors", (a) => {
    const lf = LengthFieldDecoder();
    a.eq(lf.kind, "length", "kind");
    a.eq(lf.maxFrameLength, 1048576, "default maxFrameLength 1 MiB");
    a.eq(LengthFieldDecoder({ width: 8, endian: "le", maxFrameLength: 2 ** 53 }).maxFrameLength, 2 ** 53, "2^53 accepted");
    a.throws(() => LengthFieldDecoder({ bogus: 1 }), { type: TypeError, message: 'unknown option "bogus" (valid: offset, width, endian, adjustment, initialBytesToStrip, maxFrameLength, failFast)' }, "strict bag");
    a.throws(() => LengthFieldDecoder({ width: 5 }), { type: RangeError, message: "width must be 1, 2, 3, 4 or 8" }, "width 5");
    a.eq(LengthFieldDecoder({ width: 3 }).kind, "length", "width 3 (Netty: getUnsignedMedium)");
    a.throws(() => LengthFieldDecoder({}, { bogus: 1 }), { type: TypeError, message: 'unknown option "bogus" (valid: stopOnError, rejectUnsafeLength, resumeAfterError)' }, "meta bag strict");
    a.throws(() => LengthFieldDecoder({}, { stopOnError: 1 }), { type: TypeError, message: "option stopOnError must be a boolean" }, "meta type");
    a.throws(() => LengthFieldDecoder({ failFast: "yes" }), { type: TypeError, message: "option failFast must be a boolean" }, "failFast type");
    a.throws(() => LengthFieldDecoder({}, 3), { type: TypeError, message: "options must be an object" }, "meta not an object");
    a.throws(() => DelimiterDecoder({ delimiter: "\n" }, { rejectUnsafeLength: true }), { type: TypeError, message: 'unknown option "rejectUnsafeLength" (valid: stopOnError, holdOverlapping, resumeAfterError)' }, "delimiter meta bag strict");
    a.throws(() => LengthFieldDecoder({}, { resumeAfterError: "yes" }), { type: TypeError, message: "option resumeAfterError must be a boolean" }, "resumeAfterError type");
    a.eq(DelimiterDecoder({ delimiter: "\n" }, { resumeAfterError: true }).kind, "delimiter", "delimiter resumeAfterError accepted");
    a.eq(DelimiterDecoder({ delimiter: "\n", failFast: false }, { stopOnError: true, holdOverlapping: true }).kind, "delimiter", "all delimiter options");
    a.throws(() => LengthFieldDecoder({ endian: "big" }), { type: TypeError, message: 'endian must be "be" or "le"' }, "endian");
    a.throws(() => LengthFieldDecoder({ maxFrameLength: 2 ** 53 + 2 }), RangeError, "max above 2^53");
    a.throws(() => LengthFieldDecoder({ maxFrameLength: 0 }), RangeError, "max 0");
    a.throws(() => LengthFieldDecoder({ offset: 1.5 }), RangeError, "non-integer offset");
    a.throws(() => LengthFieldDecoder({ offset: "1" }), { type: TypeError, message: "offset must be a number" }, "string offset");
    a.throws(() => LengthFieldDecoder({ offset: 10, width: 4, maxFrameLength: 13 }), { type: RangeError, message: "offset + width must not exceed maxFrameLength" }, "Netty: offset + width <= max");
    a.throws(() => LengthFieldDecoder(5), { type: TypeError, message: "options must be an object" }, "non-object bag");
    a.eq(DelimiterDecoder({ delimiter: "\n" }).kind, "delimiter", "delimiter kind");
    a.eq(DelimiterDecoder({ delimiter: ["\r\n", Uint8Array.of(10), new Uint8Array([0]).buffer] }).kind, "delimiter", "array of BytesInput");
    a.throws(() => DelimiterDecoder({}), { type: TypeError, message: "delimiter is required" }, "delimiter required");
    a.throws(() => DelimiterDecoder({ delimiter: "" }), { type: RangeError, message: "a delimiter must be 1..4096 bytes" }, "empty delimiter (Netty)");
    a.throws(() => DelimiterDecoder({ delimiter: [] }), RangeError, "empty array");
    a.throws(() => DelimiterDecoder({ delimiter: 7 }), TypeError, "number delimiter");
    a.throws(() => DelimiterDecoder({ delimiter: "\n", stripDelimiter: 1 }), { type: TypeError, message: "stripDelimiter must be a boolean" }, "strip type");
    a.throws(() => DelimiterDecoder({ delimiter: "\n", bogus: 1 }), { type: TypeError, message: 'unknown option "bogus"' }, "strict bag");
    a.eq(FixedLengthDecoder(4).kind, "fixed", "fixed kind");
    a.throws(() => FixedLengthDecoder(0), RangeError, "fixed 0");
    a.throws(() => FixedLengthDecoder("4"), TypeError, "fixed string");
    for (const [n, f] of [["LengthFieldDecoder", LengthFieldDecoder], ["DelimiterDecoder", DelimiterDecoder], ["FixedLengthDecoder", FixedLengthDecoder]])
        a.throws(() => new f({ delimiter: "x" }), { type: TypeError, message: n + " is not a constructor" }, n + " house rule");
    a.throws(() => new TCPServer({ port: 0, decoder: {} }), { type: TypeError, message: "decoder must be a FrameDecoder" }, "decoder option type");
    const srv = new TCPServer({ port: 0, decoder: FixedLengthDecoder(4) });
    a.throws(() => srv.start({ data() {} }), { type: TypeError, message: "a decoder needs a frame handler" }, "decoder without frame handler");
    srv.close();
    a.throws(() => TCPServer.connect({ host: "127.0.0.1", port: 1, decoder: FixedLengthDecoder(4) }, {}), { type: TypeError, message: "a decoder needs a frame handler" }, "client decoder without frame handler");
    a.throws(() => new TCPServer({ port: 0 }).start({ frame() {}, error() {}, bogus() {} }), { type: TypeError, message: 'unknown option "bogus" (valid: connect, data, close, drain, frame, error)' }, "handler bag strict, lists frame and error");
});

t.test("server-side LengthField over randomly segmented writes equals the reference framing", async (a) => {
    for (let round = 0; round < 3; round++) {
        const { bodies, wire } = lengthFrames(120);
        const got = [];
        let dataEvents = 0;
        const srv = serverWith({ decoder: LengthFieldDecoder({ width: 2, initialBytesToStrip: 2, maxFrameLength: 65537 }) },
            { frame(c, f) { got.push(f); }, data() { dataEvents++; } });
        const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
        await writeSegmented(c, wire, round === 0 ? 3 : 40);
        a.ok(await until(() => got.length >= bodies.length), "round " + round + ": all frames arrived");
        sameFrames(a, got, bodies, "round " + round);
        a.eq(dataEvents, 0, "data never fires while a decoder is set");
        a.ok(got.every((f) => f instanceof Uint8Array && f.byteOffset === 0 && f.buffer.byteLength === f.length), "one fresh Uint8Array per frame");
        c.close();
        srv.close();
    }
}, T);

t.test("client-side decoder option: Delimiter and FixedLength frames from the server", async (a) => {
    const lines = [];
    for (let i = 0; i < 200; i++) lines.push("line-" + i + "-" + "x".repeat(rnd(50)));
    const wire = enc(lines.join("\r\n") + "\r\n" + "tail-without-delimiter");
    let sconn;
    const srv = serverWith({}, { connect(c) { sconn = c; } });
    const got = [];
    const c = await connect({ host: "127.0.0.1", port: srv.port, decoder: DelimiterDecoder({ delimiter: ["\r\n", "\n"], maxFrameLength: 256 }) },
        { frame(cc, f) { got.push(dec(f)); } });
    await until(() => sconn);
    await writeSegmented(sconn, wire, 9);
    a.ok(await until(() => got.length >= lines.length), "all lines");
    a.eq(got.length, lines.length, "the undelimited tail is not a frame");
    a.eq(JSON.stringify(got), JSON.stringify(lines), "lines equal, \\r\\n beats \\n (shortest frame)");
    c.close();
    srv.close();

    const fixed = [];
    let s2;
    const srv2 = serverWith({}, { connect(cc) { s2 = cc; } });
    const c2 = await connect({ host: "127.0.0.1", port: srv2.port, decoder: FixedLengthDecoder(5) }, { frame(cc, f) { fixed.push(dec(f)); } });
    await until(() => s2);
    await writeSegmented(s2, enc("abcdefghijklmnopqrstuvwxy"), 3);
    a.ok(await until(() => fixed.length === 5), "five fixed frames");
    a.eq(fixed.join("|"), "abcde|fghij|klmno|pqrst|uvwxy", "fixed frames in order");
    c2.close();
    srv2.close();
}, T);

t.test("TLS: frames are decoded from plaintext, both directions", async (a) => {
    const dir = makeTempDir("frames");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(dir, "k.pem"), k.privateKey);
    writeFile(new Path(dir, "c.pem"), crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 2 }));
    const { bodies, wire } = lengthFrames(80);
    const got = [], back = [];
    const srv = serverWith({ tls: { cert: dir + "/c.pem", key: dir + "/k.pem" }, decoder: LengthFieldDecoder({ width: 2, initialBytesToStrip: 2 }) }, {
        frame(c, f) { got.push(f); c.write(concat([Uint8Array.of(f.length >> 8, f.length & 255), enc("<"), f])); },
    });
    const c = await connect({ host: "localhost", port: srv.port, tls: { ca: dir + "/c.pem" }, decoder: LengthFieldDecoder({ width: 2, adjustment: 1, initialBytesToStrip: 2 }) },
        { frame(cc, f) { back.push(f); } });
    await writeSegmented(c, wire, 25);
    a.ok(await until(() => back.length >= bodies.length, 10000), "echoes arrived");
    sameFrames(a, got, bodies, "TLS server frames");
    sameFrames(a, back, bodies.map((b) => concat([enc("<"), b])), "TLS client frames (lengthAdjustment 1 covers the marker byte)");
    c.close();
    srv.close();
    removeAll(dir);
}, T);

t.test("oversize frame, Netty default: error event, the frame is discarded, decoding goes on, the connection stays open", async (a) => {
    const got = [], order = [];
    let closed = false;
    const srv = serverWith({ decoder: LengthFieldDecoder({ width: 2, initialBytesToStrip: 2, maxFrameLength: 102 }) }, {
        frame(c, f) { got.push(f.length); order.push("frame"); },
        error(c, e) { order.push(e); },
        close() { closed = true; },
    });
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
    const ok100 = concat([Uint8Array.of(0, 100), new Uint8Array(100)]);
    const bad = concat([Uint8Array.of(0, 101), new Uint8Array(101).fill(7)]);
    await writeSegmented(c, concat([ok100, ok100, bad, ok100]), 7);
    await tick(50);
    a.eq(closed, false, "a decoder error does not close the connection");
    c.end();
    a.ok(await until(() => closed), "closed after the client's FIN");
    a.eq(got.join(","), "100,100,100", "the 103-byte frame is discarded whole; the frame after it is delivered (at the latest at end of input)");
    a.eq(order.join(","), "frame,frame,frame decoder: Adjusted frame length exceeds 102: 103 - discarded,frame", "order and Netty's message");
    srv.close();

    const errs2 = [], got2 = [];
    let s2;
    const srv2 = serverWith({ decoder: DelimiterDecoder({ delimiter: "\n", maxFrameLength: 8 }) }, {
        connect(cc) { s2 = cc; }, frame(cc, f) { got2.push(dec(f)); }, error(cc, e) { errs2.push(e); },
    });
    const c2 = await connect({ host: "127.0.0.1", port: srv2.port }, {});
    await writeSegmented(c2, enc("0123456789abcdef"), 2);
    a.ok(await until(() => errs2.length === 1), "delimiter overflow: one error (failFast)");
    a.ok(/^frame decoder: frame length exceeds 8: \d+ - discarded$/.test(errs2[0]), "Netty message: " + errs2[0]);
    c2.write("tail\nok\n");
    a.ok(await until(() => got2.length === 1), "the discard ends at the next delimiter silently, then frames again");
    a.eq(got2[0], "ok", "ok after the discarded line");
    c2.close();
    srv2.close();

    const errs4 = [], got4 = [];
    const srv4 = serverWith({ decoder: DelimiterDecoder({ delimiter: ["\r\n", "\n"], maxFrameLength: 4 }) }, {
        frame(cc, f) { got4.push(dec(f)); }, error(cc, e) { errs4.push(e); },
    });
    const c4 = await connect({ host: "127.0.0.1", port: srv4.port }, {});
    c4.write("abcdefgh");
    a.ok(await until(() => errs4.length === 1), "line mode error");
    a.ok(/^frame decoder: frame length \(over \d+\) exceeds the allowed maximum \(4\)$/.test(errs4[0]), "{\\r\\n,\\n} is LineBasedFrameDecoder: " + errs4[0]);
    c4.write("\r\nab\r\n");
    a.ok(await until(() => got4.length === 1), "line frame after the discard");
    a.eq(got4[0], "ab", "ab");
    c4.close();
    srv4.close();

    const errs5 = [];
    const w8 = Uint8Array.of(255, 255, 255, 255, 255, 255, 255, 255);
    for (const [meta, want] of [[undefined, "frame decoder: negative pre-adjustment length field: -1"],
        [{ rejectUnsafeLength: true }, "frame decoder: length field 18446744073709551615 exceeds 2^53 (rejectUnsafeLength)"]]) {
        const srv5 = serverWith({ decoder: LengthFieldDecoder({ width: 8 }, meta) }, { frame() {}, error(cc, e) { errs5.push(e); } });
        const c5 = await connect({ host: "127.0.0.1", port: srv5.port }, {});
        c5.write(w8);
        a.ok(await until(() => errs5.length > 0), "width 8 error");
        a.eq(errs5.pop(), want, "width 8 all ones: " + (meta ? "rejectUnsafeLength" : "Netty getLong is -1"));
        c5.close();
        srv5.close();
    }
}, T);

t.test("stopOnError (meta): frames before the error, then error, then close, no partial frame", async (a) => {
    const got3 = [], order = [];
    const srv3 = serverWith({ decoder: LengthFieldDecoder({ width: 1, initialBytesToStrip: 1, maxFrameLength: 10 }, { stopOnError: true }) }, {
        frame(c, f) { got3.push(dec(f)); order.push("frame"); }, error() { order.push("error"); }, close() { order.push("close"); },
    });
    const c3 = await connect({ host: "127.0.0.1", port: srv3.port }, {});
    c3.write(concat([Uint8Array.of(3), enc("abc"), Uint8Array.of(3), enc("def"), Uint8Array.of(20), enc("zzzz")]));
    a.ok(await until(() => order.includes("close")), "stopOnError: one read holding good frames and a bad header closes");
    a.eq(got3.join("|"), "abc|def", "the frames decoded before the bad header in the same read are delivered");
    a.eq(order.join(","), "frame,frame,error,close", "frames, then error, then close");
    srv3.close();

    const errs = [];
    let closed = false;
    const srv = serverWith({ decoder: DelimiterDecoder({ delimiter: "\n", maxFrameLength: 8, failFast: false }, { stopOnError: true }) }, {
        frame() {}, error(c, e) { errs.push(e); }, close() { closed = true; },
    });
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
    c.write("0123456789abcdef");
    a.ok(await until(() => closed), "stopOnError + failFast false: closes at detection, not after a later delimiter");
    a.eq(errs.length, 1, "one error");
    srv.close();
}, T);

t.test("failFast (Netty option): true reports a too-long frame at detection, false only once its bytes are discarded", async (a) => {
    for (const ff of [true, false]) {
        const errs = [], got = [];
        const srv = serverWith({ decoder: LengthFieldDecoder({ width: 1, initialBytesToStrip: 1, maxFrameLength: 10, failFast: ff }) }, {
            frame(c, f) { got.push(dec(f)); }, error(c, e) { errs.push(e); },
        });
        const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
        c.write(concat([Uint8Array.of(20), enc("12345")]));
        if (ff) a.ok(await until(() => errs.length === 1), "LengthField failFast true: error after the header alone");
        else { await tick(150); a.eq(errs.length, 0, "LengthField failFast false: no error while 15 bytes remain to discard"); }
        c.write(concat([enc("678901234567890"), Uint8Array.of(2), enc("ok")]));
        a.ok(await until(() => errs.length === 1), "LengthField failFast " + ff + ": the error");
        if (!ff) { await tick(100); a.eq(got.length, 0, "failFast false throws when the discard ends: as in Netty's callDecode, ok waits for the next read"); }
        c.write(concat([Uint8Array.of(1), enc("!")]));
        a.ok(await until(() => got.length === 2), "LengthField failFast " + ff + ": the next frames");
        a.eq(errs.join("|"), "frame decoder: Adjusted frame length exceeds 10: 21 - discarded", "LengthField failFast " + ff + ": one error, Netty's message");
        a.eq(got.join("|"), "ok|!", "frames after the discarded one, in order");
        c.close();
        srv.close();

        const derrs = [], dgot = [];
        const dsrv = serverWith({ decoder: DelimiterDecoder({ delimiter: "\n", maxFrameLength: 8, failFast: ff }) }, {
            frame(cc, f) { dgot.push(dec(f)); }, error(cc, e) { derrs.push(e); },
        });
        const dc = await connect({ host: "127.0.0.1", port: dsrv.port }, {});
        dc.write("0123456789abcdef");
        if (ff) a.ok(await until(() => derrs.length === 1), "Delimiter failFast true: error before any delimiter");
        else { await tick(150); a.eq(derrs.length, 0, "Delimiter failFast false: no error until the delimiter ends the discard"); }
        dc.write("gh\nok\n");
        a.ok(await until(() => derrs.length === 1), "Delimiter failFast " + ff + ": the error");
        if (!ff) { await tick(100); a.eq(dgot.length, 0, "failFast false throws at the delimiter: ok waits for the next read"); }
        dc.write("z\n");
        a.ok(await until(() => dgot.length === 2), "Delimiter failFast " + ff + ": the next frames");
        a.eq(derrs.length, 1, "Delimiter failFast " + ff + ": one error");
        a.ok(/^frame decoder: frame length exceeds 8: 16 - discarded$/.test(derrs[0]), "Netty reports tooLongFrameLength, the bytes skipped before the delimiter read: " + derrs[0]);
        a.eq(dgot.join("|"), "ok|z", "frames after the discarded line, in order");
        dc.close();
        dsrv.close();
    }
}, T);

t.test("resumeAfterError (meta): the bytes after an error in the same read decode at once; default waits for the next read (Netty)", async (a) => {
    for (const resume of [false, true]) {
        const errs = [], got = [];
        const srv = serverWith({ decoder: LengthFieldDecoder({ width: 1, initialBytesToStrip: 1, maxFrameLength: 10, failFast: false }, { resumeAfterError: resume }) }, {
            frame(c, f) { got.push(dec(f)); }, error(c, e) { errs.push(e); },
        });
        const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
        c.write(concat([Uint8Array.of(20), enc("12345")]));
        await tick(50);
        c.write(concat([enc("678901234567890"), Uint8Array.of(2), enc("ok")]));
        a.ok(await until(() => errs.length === 1), "LengthField resumeAfterError " + resume + ": the error");
        if (resume) a.ok(await until(() => got.length === 1), "resumeAfterError: ok arrives with no further bytes from the peer");
        else { await tick(150); a.eq(got.length, 0, "default: ok waits after the thrown error"); }
        a.eq(got.join("|"), resume ? "ok" : "", "frames before any further write");
        c.close();
        srv.close();

        const derrs = [], dgot = [];
        const dsrv = serverWith({ decoder: DelimiterDecoder({ delimiter: "\n", maxFrameLength: 3 }, { resumeAfterError: resume }) }, {
            frame(cc, f) { dgot.push(dec(f)); }, error(cc, e) { derrs.push(e); },
        });
        const dc = await connect({ host: "127.0.0.1", port: dsrv.port }, {});
        dc.write("abcdef\nok\n");
        a.ok(await until(() => derrs.length === 1), "Delimiter resumeAfterError " + resume + ": the error");
        if (resume) a.ok(await until(() => dgot.length === 1), "resumeAfterError: ok in the same read as the too-long frame");
        else { await tick(150); a.eq(dgot.length, 0, "default: ok waits after the thrown error"); }
        dc.write("z\n");
        a.ok(await until(() => dgot.length === 2), "the next read delivers the rest in both modes");
        a.eq(dgot.join("|"), "ok|z", "order kept");
        dc.close();
        dsrv.close();
    }
}, T);

t.test("pause() inside a frame handler: decoded frames wait, none lost, order kept", async (a) => {
    if (URING) return a.skip("pause() is a no-op on the io_uring backend (d.ts TCPConn.pause)");
    const got = [];
    let sc, atPause = -1;
    const srv = serverWith({ decoder: FixedLengthDecoder(4) }, {
        connect(c) { sc = c; },
        frame(c, f) { got.push(dec(f)); if (got.length === 3) { c.pause(); atPause = got.length; } },
    });
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
    const all = [];
    for (let i = 0; i < 50; i++) all.push(String(i).padStart(4, "0"));
    c.write(all.join(""));
    await until(() => atPause > 0);
    await tick(150);
    a.eq(got.length, 3, "no frame while paused, though the chunk held more");
    a.eq(sc.paused, true, "paused");
    sc.resume();
    a.ok(await until(() => got.length === 50), "all frames after resume");
    a.eq(got.join(""), all.join(""), "in order, none lost");
    c.close();
    srv.close();
}, T);

t.test("EOF: a held delimiter frame is resolved (decodeLast); a partial length frame is dropped", async (a) => {
    const got = [];
    let closed = false;
    const srv = serverWith({ decoder: DelimiterDecoder({ delimiter: ["bb", "b"] }, { holdOverlapping: true }) }, {
        frame(c, f) { got.push(dec(f)); }, close() { closed = true; },
    });
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
    c.write("1bb2b3ab");
    await until(() => got.length >= 2);
    await tick(60);
    a.eq(got.join("|"), "1|2", "3a is held: a following b would make bb win and give 3a then nothing");
    c.end();
    a.ok(await until(() => closed), "closed after EOF");
    a.eq(got.join("|"), "1|2|3a", "EOF resolves the held frame as Netty on the whole stream");
    srv.close();

    const got2 = [];
    let closed2 = false, errs = 0;
    const srv2 = serverWith({ decoder: LengthFieldDecoder({ width: 1, initialBytesToStrip: 1 }) }, {
        frame(cc, f) { got2.push(dec(f)); }, error() { errs++; }, close() { closed2 = true; },
    });
    const c2 = await connect({ host: "127.0.0.1", port: srv2.port }, {});
    c2.write(concat([Uint8Array.of(2), enc("ok"), Uint8Array.of(9), enc("part")]));
    await tick(30);
    c2.end();
    a.ok(await until(() => closed2), "closed after EOF");
    a.eq(got2.join("|"), "ok", "the incomplete tail is never delivered");
    a.eq(errs, 0, "an incomplete tail at EOF is not an error");
    srv2.close();
}, T);

t.test("setDecoder on a connection whose bytes already wait in the kernel; ownership TypeErrors", async (a) => {
    if (URING) return a.skip("pause() is a no-op on the io_uring backend");
    const got = [];
    let sc, dataEvents = 0;
    const srv = serverWith({}, {
        connect(c) { sc = c; c.pause(); },
        data() { dataEvents++; },
        frame(c, f) { got.push(dec(f)); },
    });
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
    c.write("alpha\nbeta\ngam");
    await tick(30);
    c.write("ma\ndelta\n");
    await tick(100);
    await until(() => sc);
    const d = DelimiterDecoder({ delimiter: "\n" });
    sc.setDecoder(d);
    a.throws(() => sc.setDecoder(d), { type: TypeError, message: "already has a frame decoder" }, "one decoder per connection");
    a.throws(() => sc.streams(), { type: TypeError, message: "streams() is byte-mode only" }, "streams() refused with a decoder");
    sc.resume();
    a.ok(await until(() => got.length === 4), "frames from the waiting bytes");
    a.eq(got.join("|"), "alpha|beta|gamma|delta", "decoded from the first byte");
    a.eq(dataEvents, 0, "no data event");
    a.throws(() => sc.setDecoder({}), { type: TypeError, message: "decoder must be a FrameDecoder" }, "not a FrameDecoder");
    c.close();
    srv.close();

    let s2;
    const srv2 = serverWith({}, { connect(cc) { s2 = cc; cc.streams(); }, frame() {} });
    const c2 = await connect({ host: "127.0.0.1", port: srv2.port }, {});
    await until(() => s2);
    a.throws(() => s2.setDecoder(FixedLengthDecoder(2)), { type: TypeError, message: "streams() owns this connection" }, "setDecoder after streams()");
    c2.close();
    srv2.close();

    let s3;
    const srv3 = serverWith({}, { connect(cc) { s3 = cc; } });
    const c3 = await connect({ host: "127.0.0.1", port: srv3.port }, {});
    await until(() => s3);
    a.throws(() => s3.setDecoder(FixedLengthDecoder(2)), { type: TypeError, message: "the handlers have no frame function" }, "setDecoder without a frame handler");
    c3.close();
    srv3.close();
}, T);

t.test("a frame handler that closes the connection stops delivery", async (a) => {
    const got = [];
    let closes = 0;
    const srv = serverWith({ decoder: FixedLengthDecoder(1) }, {
        frame(c, f) { got.push(f[0]); if (got.length === 2) c.close(); }, close() { closes++; },
    });
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {});
    c.write("abcdef");
    a.ok(await until(() => closes === 1), "closed once");
    await tick(30);
    a.eq(got.length, 2, "no frame after close() in the handler");
    srv.close();
}, T);

function tlsDir() {
    const dir = makeTempDir("frames-tls");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(dir, "k.pem"), k.privateKey);
    writeFile(new Path(dir, "c.pem"), crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 2 }));
    return dir;
}

t.test("TLS: held frame resolved at FIN, pause/resume inside one decrypted read", async (a) => {
    const dir = tlsDir();
    const tls = { cert: dir + "/c.pem", key: dir + "/k.pem" };
    const got = [];
    let closed = false;
    const srv = serverWith({ tls, decoder: DelimiterDecoder({ delimiter: ["bb", "b"] }, { holdOverlapping: true }) }, {
        frame(c, f) { got.push(dec(f)); }, close() { closed = true; },
    });
    const c = await connect({ host: "localhost", port: srv.port, tls: { ca: dir + "/c.pem" } }, {});
    c.write("1bb2b3ab");
    await until(() => got.length >= 2);
    await tick(60);
    a.eq(got.join("|"), "1|2", "TLS: 3a held");
    c.end();
    a.ok(await until(() => closed), "TLS: closed after FIN");
    a.eq(got.join("|"), "1|2|3a", "TLS: FIN resolves the held frame");
    srv.close();

    if (URING) { removeAll(dir); return a.skip("pause() is a no-op on the io_uring backend"); }
    const fr = [];
    let sc, paused = false;
    const srv2 = serverWith({ tls, decoder: FixedLengthDecoder(4) }, {
        connect(cc) { sc = cc; },
        frame(cc, f) { fr.push(dec(f)); if (fr.length === 3) { cc.pause(); paused = true; } },
    });
    const c2 = await connect({ host: "localhost", port: srv2.port, tls: { ca: dir + "/c.pem" } }, {});
    const all = [];
    for (let i = 0; i < 60; i++) all.push(String(i).padStart(4, "0"));
    c2.write(all.join(""));
    await until(() => paused);
    await tick(150);
    a.eq(fr.length, 3, "TLS: no frame while paused");
    sc.resume();
    a.ok(await until(() => fr.length === 60), "TLS: all frames after resume");
    a.eq(fr.join(""), all.join(""), "TLS: order kept, none lost");
    c2.close();
    srv2.close();
    removeAll(dir);
}, T);

t.test("TLS close_notify from openssl s_client resolves a held frame like a FIN", async (a) => {
    if (!Which("openssl")) return a.skip("openssl(1) missing");
    const dir = tlsDir();
    const got = [];
    let closed = false;
    const srv = serverWith({ tls: { cert: dir + "/c.pem", key: dir + "/k.pem" }, decoder: DelimiterDecoder({ delimiter: ["bb", "b"] }, { holdOverlapping: true }) }, {
        frame(c, f) { got.push(dec(f)); }, close() { closed = true; },
    });
    const h = spawnHelper("(printf '1bb2b3ab'; sleep 0.5) | openssl s_client -connect 127.0.0.1:" + srv.port +
        " -servername localhost -no_ign_eof > " + dir + "/sc.log 2>&1");
    a.ok(await until(() => closed, 15000), "closed after s_client ends");
    const log = readFile(new Path(dir + "/sc.log"), "utf8");
    a.ok(/CONNECTED|Cipher/.test(log), "s_client completed a handshake");
    a.eq(got.join("|"), "1|2|3a", "close_notify (or the FIN after it) resolves the held frame");
    h.stop();
    srv.close();
    removeAll(dir);
}, T);

t.test("stopOnError: the error handler fires once even when it calls pause() and resume()", async (a) => {
    const errs = [];
    let closed = false;
    const srv2 = serverWith({ decoder: LengthFieldDecoder({ width: 1, maxFrameLength: 4 }, { stopOnError: true }) }, {
        frame() {},
        error(c, e) { errs.push(e); if (!URING) { c.pause(); c.resume(); } },
        close() { closed = true; },
    });
    const c = await connect({ host: "127.0.0.1", port: srv2.port }, {});
    c.write(Uint8Array.of(1, 65, 9, 1, 2, 3));
    a.ok(await until(() => closed), "closed");
    await tick(30);
    a.eq(errs.length, 1, "one error event");
    srv2.close();
}, T);

t.test("client-side setDecoder from the connect handler (the route for connectHappy)", async (a) => {
    let sc;
    const srv = serverWith({}, { connect(cc) { sc = cc; } });
    const got = [];
    let data = 0;
    const c = await connect({ host: "127.0.0.1", port: srv.port }, {
        connect(cc) { cc.setDecoder(FixedLengthDecoder(3)); },
        frame(cc, f) { got.push(dec(f)); },
        data() { data++; },
    });
    await until(() => sc);
    await writeSegmented(sc, enc("abcdefghi"), 2);
    a.ok(await until(() => got.length === 3), "three frames");
    a.eq(got.join("|"), "abc|def|ghi", "client frames");
    a.eq(data, 0, "no data events on the client");
    c.close();
    srv.close();
}, T);

await t.run();
