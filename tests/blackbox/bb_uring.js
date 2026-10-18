// bb_uring.js -- black-box contract tests for dyna:uring (the Linux-only
// io_uring bulk reader).
//
// Contract: dynajs.d.ts lines 5832-5954 (the "dyna:uring" module section).
// Slice used: /tmp/dyna_contract/uring_url.d.ts.
//
// Linux-only module: on any platform (or build) without dyna:uring this file
// prints "bb_uring: SKIP(platform) ALL-SKIP" and exits 0.
//
// FIXTURE BASIS (hand-computed, no external runtime): 32-bit FNV-1a rolling
// checksum -- hash starts at the offset basis 2166136261 (0x811c9dc5) and per
// byte does hash = (hash XOR byte) * 16777619 (0x01000193) mod 2^32, per the
// FNV-1a reference (draft-eastlake-fnv / IETF FNV test vectors):
//   FNV-1a32("") = 0x811c9dc5 = 2166136261  (basis, zero rounds)
//   FNV-1a32("a") = 0xe40c292c = 3826002220  (hand-checked: basis^0x61 =
//   0x811c9da4; *prime mod 2^32 = 0xe40c292c)
// Fixture bytes hand-derived from ASCII/UTF-8:
//   "Dyna" = [0x44,0x79,0x6e,0x61]; "é" = [0xC3,0xA9]; "→" = [0xE2,0x86,0x92].

// Static import (test-generation slip fix): a file with no static import is
// parsed as a SCRIPT where `await import(...)` mid-file is a SyntaxError, and
// this engine only accepts static imports in the file PROLOGUE (before any
// other statement). A static import makes this a module (top-level await
// legal) without forcing dyna:uring to resolve -- that stays dynamic so macOS
// can ALL-SKIP. dyna:file is the canonical filesystem door, used ONLY to
// build and clean up temp fixtures, and exists everywhere.
import * as fileMod from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); }

function runTable(tname, rows, fn) {
    for (const row of rows) {
        try { fn(row); }
        catch (e) { throw new Error("table " + tname + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
    }
}

// UTF-8 byte length, hand-derived from code points (1/2/3/4 byte shapes), so
// the writeText check holds for the multibyte rows too: the d.ts File.writeText
// contract is "returns the byte count", which only equals the string's UTF-16
// length when every char is ASCII ("héllo→ wörld\n" is 13 chars but 17 bytes).
function utf8ByteLen(s) {
    let b = 0;
    for (const ch of s) {
        const c = ch.codePointAt(0);
        b += c <= 0x7f ? 1 : c <= 0x7ff ? 2 : c <= 0xffff ? 3 : 4;
    }
    return b;
}

let uring = null;
try {
    uring = await import("dyna:uring");
    const { Path, File } = fileMod;

    const uniq = "bb_uring_" + Date.now() + "_" + Math.floor(Math.random() * 1e9);
    const paths = [];
    function mkPath(tag) { const pth = Path.temp().join(uniq + "_" + tag); paths.push(pth); return pth; }
    function writeFixture(pth, s) { return new File(pth).writeText(s); } // returns byte count

    const BIG = "abcdefgh".repeat(8192); // 64 KiB ASCII

    try {
        // --------------------------------------------------------------
        // T1: readFile vs readFileSync byte-equality parity, plus exact
        // content round trip (slice: readFile = io_uring bulk reader,
        // readFileSync = blocking pread(2) reference reader)
        // --------------------------------------------------------------
        runTable("parity", [
            ["empty file", ""],
            ["ascii one line", "hello uring\n"],
            ["utf8 multibyte", "héllo→ wörld\n"],
            ["newlines only", "\n\n\n"],
            ["64KiB pattern", BIG],
        ], row => {
            const pth = mkPath("parity");
            const wrote = writeFixture(pth, row[1]);
            assertEq(wrote, utf8ByteLen(row[1]), "writeText byte count matches UTF-8 byte length");
            assertEq(uring.readFile(pth), row[1], "readFile round trip");
            assertEq(uring.readFileSync(pth), row[1], "readFileSync round trip");
            assertEq(uring.readFile(pth), uring.readFileSync(pth), "readFile === readFileSync");
        });

        // --------------------------------------------------------------
        // T2: readFileBytes -- exact bytes, no UTF-8 decode; empty file =
        // zero-length array (slice pin)
        // --------------------------------------------------------------
        runTable("readFileBytes", [
            ["ascii exact bytes", "Dyna", [68, 121, 110, 97]],
            ["empty file is a zero-length array", "", []],
            ["utf8 bytes are raw, not decoded", "é→", [195, 169, 226, 134, 146]],
        ], row => {
            const pth = mkPath("bytes");
            writeFixture(pth, row[1]);
            const got = uring.readFileBytes(pth);
            assert(got instanceof Uint8Array, "readFileBytes returns Uint8Array");
            assertDeepEq(Array.from(got), row[2], "exact byte content");
        });

        // --------------------------------------------------------------
        // T3: checksum -- {bytes, sum} shape, FNV-1a hand-computed rows,
        // useUring parity (slice: useUring selects the reader, default
        // true; 32-bit FNV-1a rolling checksum)
        // --------------------------------------------------------------
        runTable("checksum", [
            ["result shape {bytes,sum} are integers", "abc", "shape"],
            // FNV-1a basis: zero rounds leave the offset basis untouched.
            ["empty file -> bytes 0, sum = offset basis", "", { bytes: 0, sum: 2166136261 }],
            // FNV-1a reference vector, hand-checked (see file header).
            ["FNV-1a32('a') = 0xe40c292c", "a", { bytes: 1, sum: 3826002220 }],
            ["useUring true vs false agree", "parity-content\n", "parity"],
            ["bytes matches writeText byte count", "countme", "count"],
            ["bytes matches readFileBytes length", BIG, "length"],
        ], row => {
            const pth = mkPath("cksum");
            const wrote = writeFixture(pth, row[1]);
            if (row[2] === "shape") {
                const r = uring.checksum(pth);
                assertEq(typeof r, "object", "checksum returns an object");
                assertEq(typeof r.bytes, "number", "checksum.bytes is a number");
                assertEq(typeof r.sum, "number", "checksum.sum is a number");
                assert(Number.isInteger(r.bytes) && Number.isInteger(r.sum), "bytes/sum are integers");
            } else if (row[2] === "parity") {
                assertDeepEq(uring.checksum(pth, true), uring.checksum(pth, false), "useUring=true === useUring=false");
            } else if (row[2] === "count") {
                assertEq(uring.checksum(pth).bytes, wrote, "checksum.bytes === writeText count");
            } else if (row[2] === "length") {
                assertEq(uring.checksum(pth).bytes, uring.readFileBytes(pth).length, "checksum.bytes === readFileBytes length");
            } else {
                const r = uring.checksum(pth);
                assertEq(r.bytes, row[2].bytes, "checksum.bytes");
                assertEq(r.sum, row[2].sum, "checksum.sum");
            }
        });

    } finally {
        for (const pth of paths) { try { new File(pth).remove(); } catch (e) {} }
    }

    print("bb_uring: all tests passed (" + n + " assertions)");

} catch (e) {
    if (!uring) {
        // dyna:uring not present on this platform/build (e.g. macOS)
        print("bb_uring: SKIP(platform) ALL-SKIP");
    } else {
        // module exists: a test failure must not masquerade as a platform skip
        throw e;
    }
}
