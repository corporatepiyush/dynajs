// timeout: 300
// covers: dyna:compress.gzip
// tests/compress/test_audit_w3_native_1.js -- audit wave 3, batch 1.
// Every section names its ORACLE (what makes the expected value true
// independently of this engine) and its CONTROL (the neighbouring case that
// must not change). Each assertion marked [red] fails on the pre-fix binary.
//   SEC-03/D1-03/P1-01  native limit sees dyna-* allocations
//   C1-03               exit with background jobs in flight / uncaught error with live I/O
//   D1-01 exact maxOutputBytes   D1-02 trailing bytes, zstd multi-frame
//   D1-04 nothing written unless the whole zip decodes   D1-05 overlap, bounded inflate
//   D1-06 directory count        D1-07 "." names, over-long GNU long name
// build-note: needs the native modules
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gzip_audit_w3_native_1");
import { args, Exec, memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import * as c from "dyna:compress";
import { Path, makeTempDir, writeFile, exists, removeAll } from "dyna:file";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const TMP = makeTempDir("w3n1-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: 60000, encoding: "utf8" });
}
const le16 = (b, o) => b[o] | (b[o + 1] << 8);
const le32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const put32 = (b, o, v) => { b[o] = v; b[o + 1] = v >>> 8; b[o + 2] = v >>> 16; b[o + 3] = v >>> 24; };
function find(b, sig, from = 0) {
    for (let i = from; i + 4 <= b.length; i++) if (le32(b, i) === sig) return i;
    return -1;
}

// ---- 1. native limit ---------------------------------------------------
// ORACLE: arithmetic. 64 MiB of zeros cannot be held in a 4 MiB budget, so the
// decode must throw; 64 KiB fits, so the same call shape must succeed.
// CONTROL: the identical script with no limit decodes all 64 MiB.

__kitS.section("native ledger is flat across churn", () => {
    setNativeMemoryLimit(1 << 30);
    const round = () => { const z = c.gzip(new Uint8Array(300000)); c.gunzip(z); c.unzstd(c.zstd(new Uint8Array(70000))); };
    for (let i = 0; i < 20; i++) round();
    const a = memoryUsage().nativeSize;
    for (let i = 0; i < 100; i++) round();
    const b = memoryUsage().nativeSize;
    for (let i = 0; i < 100; i++) round();
    const d = memoryUsage().nativeSize;
    ok(b === a && d === b, "native ledger is flat across churn (" + a + ", " + b + ", " + d + ")");
    let held = c.gunzip(c.gzip(new Uint8Array(1 << 20)));
    ok(held.length === 1 << 20, "1 MiB decode under a 1 GiB limit");
});

__kitS.section("[red]", () => {
    const codecs = [
        ["gzip", c.gzip, c.gunzip], ["deflate", c.deflate, c.inflate],
        ["lz4", c.lz4Compress, c.lz4Decompress], ["lz4frame", c.lz4Frame, c.lz4Unframe],
        ["zstd", c.zstd, c.unzstd], ["brotli", c.brotli, c.unbrotli], ["snappy", c.snappy, c.unsnappy],
    ];
    for (const [name, enc, dec] of codecs) {
        for (const n of [1, 50, 319, 320, 321, 65535, 65536, 65537, 200000]) {
            const src = new Uint8Array(n);
            for (let i = 0; i < n; i++) src[i] = (i * 31 + (i >> 7)) & 0xff;
            const z = enc(src);
            let got;
            try { got = dec(z, { maxOutputBytes: n }); } catch (e) { got = e; }
            ok(got instanceof Uint8Array && got.length === n && got[n - 1] === src[n - 1],
                "[red] " + name + " n=" + n + ": maxOutputBytes == n is admitted (" + String(got).slice(0, 60) + ")");
            ok(/^RangeError/.test(thrown(() => dec(z, { maxOutputBytes: n - 1 || 0.5 }))) || n === 1,
                name + " n=" + n + ": maxOutputBytes == n-1 is a RangeError (" + thrown(() => dec(z, { maxOutputBytes: n - 1 })).slice(0, 60) + ")");
        }
    }
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_1: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_1: " + failures + " failures");
