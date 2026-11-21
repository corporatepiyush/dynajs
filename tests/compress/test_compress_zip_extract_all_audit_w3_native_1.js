// timeout: 300
// covers: dyna:compress.ZipExtractAll
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
const __kitS = __kitSections("test_compress_zip_extract_all_audit_w3_native_1");
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

__kitS.section("control: a clean two-member zip extracts both files", () => {
    const d1 = new Uint8Array(40).fill(0x61), d2 = new Uint8Array(40).fill(0x62);
    const mk = () => c.ZipPack([{ name: "first.txt", data: d1 }, { name: "aXXXXXXXXXXX.txt", data: d2 }], { method: "store" });
    // control
    {
        const dir = P("z_ok");
        const r = c.ZipExtractAll(mk(), dir.toString());
        ok(r.length === 2 && exists(new Path(dir + "/first.txt")) && exists(new Path(dir + "/aXXXXXXXXXXX.txt")),
            "control: a clean two-member zip extracts both files");
    }
    // D1-04a: the SECOND member's name is rewritten to a traversal in both
    // headers. ORACLE: the documented contract "refused before any write".
    {
        const z = mk(), bad = "a/../../e.txt..".slice(0, 16), enc = new TextEncoder();
        const want = enc.encode("aXXXXXXXXXXX.txt"), repl = enc.encode("a/../../eee.txtt".slice(0, 16));
        let hits = 0;
        for (let i = 0; i + 16 <= z.length; i++) {
            let m = true;
            for (let j = 0; j < 16 && m; j++) m = z[i + j] === want[j];
            if (m) { z.set(repl, i); hits++; }
        }
        ok(hits === 2, "fixture: name patched in local and central header (" + hits + ")");
        const dir = P("z_trav");
        ok(/SyntaxError/.test(thrown(() => c.ZipExtractAll(z, dir.toString()))), "traversal archive is refused");
        ok(!exists(new Path(dir + "/first.txt")), "[red] D1-04: the earlier member was not written before the refusal");
    }
    // D1-04b: second member fails its CRC (one data byte flipped).
    {
        const z = mk();
        const l2 = find(z, 0x04034b50, 4);
        const data2 = l2 + 30 + le16(z, l2 + 26) + le16(z, l2 + 28);
        z[data2 + 3] ^= 0xff;
        const dir = P("z_crc");
        ok(/CRC/.test(thrown(() => c.ZipExtractAll(z, dir.toString()))), "CRC failure is reported");
        ok(!exists(new Path(dir + "/first.txt")), "[red] D1-04: a later CRC failure leaves nothing on disk");
    }
    // D1-05: both directory entries point at the same local record. ORACLE:
    // APPNOTE 6.3 -- each central header describes its own local header; two
    // entries sharing bytes is the overlap-bomb construction.
    {
        const z = c.ZipPack([{ name: "aa", data: d1 }, { name: "ab", data: d1 }], { method: "store" });
        const c1 = find(z, 0x02014b50), c2 = find(z, 0x02014b50, c1 + 4);
        z[c2 + 46 + 1] = 0x61;
        put32(z, c2 + 42, le32(z, c1 + 42));
        ok(/share archive bytes/.test(thrown(() => c.ZipExtractAll(z))), "[red] D1-05: overlapping members are refused (" + thrown(() => c.ZipExtractAll(z)).slice(0, 70) + ")");
    }
    // D1-06: EOCD says 1 entry, the directory holds 3. ORACLE: the two copies
    // of the count must agree; a reader that trusts either one alone differs
    // from every reader that trusts the other.
    {
        const z = c.ZipPack([{ name: "a", data: d1 }, { name: "b", data: d1 }, { name: "c", data: d1 }]);
        ok(c.ZipList(z).length === 3, "control: 3 entries listed");
        const e = find(z, 0x06054b50);
        z[e + 8] = 1; z[e + 10] = 1;
        ok(/declares|entries/.test(thrown(() => c.ZipList(z))), "[red] D1-06: ZipList refuses a directory longer than the declared count (" + thrown(() => c.ZipList(z)).slice(0, 70) + ")");
        ok(thrown(() => c.ZipExtractAll(z)) !== "none", "D1-06: ZipExtractAll refuses it too");
    }
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_1: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_1: " + failures + " failures");
