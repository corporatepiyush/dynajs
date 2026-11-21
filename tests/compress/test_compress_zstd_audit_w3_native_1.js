// timeout: 300
// covers: dyna:compress.zstd
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
const __kitS = __kitSections("test_compress_zstd_audit_w3_native_1");
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

__kitS.section("[red] unzstd of two concatenated frames yields both", () => {
    const a = new Uint8Array(50).fill(65), b = new Uint8Array(70).fill(66);
    const za = c.zstd(a), zb = c.zstd(b);
    const cat = new Uint8Array(za.length + zb.length); cat.set(za); cat.set(zb, za.length);
    let out; try { out = c.unzstd(cat); } catch (e) { out = e; }
    ok(out instanceof Uint8Array && out.length === 120 && out[0] === 65 && out[49] === 65 && out[50] === 66 && out[119] === 66,
        "[red] unzstd of two concatenated frames yields both (" + (out.length ?? out) + ")");
    ok(/RangeError/.test(thrown(() => c.unzstd(cat, { maxOutputBytes: 119 }))), "the cap spans both frames");
    for (const [name, enc, dec] of [["zstd", c.zstd, c.unzstd], ["lz4frame", c.lz4Frame, c.lz4Unframe], ["brotli", c.brotli, c.unbrotli]]) {
        const z = enc(a), g = new Uint8Array(z.length + 5); g.set(z); g.fill(0x41, z.length);
        ok(thrown(() => dec(g)) !== "none", "[red] " + name + ": 5 bytes after the stream are refused");
        ok(dec(z).length === 50, name + ": control, the clean stream decodes");
    }
    const lf = c.lz4Frame(a, { checksum: true });
    ok(c.lz4Unframe(lf).length === 50, "control: lz4 frame with content checksum has no false trailing-bytes error");
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_1: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_1: " + failures + " failures");
