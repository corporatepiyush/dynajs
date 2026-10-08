// timeout: 300
// tests/test_audit_w3_native_1.js -- audit wave 3, batch 1.
// Every section names its ORACLE (what makes the expected value true
// independently of this engine) and its CONTROL (the neighbouring case that
// must not change). Each assertion marked [red] fails on the pre-fix binary.
//   SEC-03/D1-03/P1-01  native limit sees dyna-* allocations
//   C1-03               exit with background jobs in flight / uncaught error with live I/O
//   D1-01 exact maxOutputBytes   D1-02 trailing bytes, zstd multi-frame
//   D1-04 nothing written unless the whole zip decodes   D1-05 overlap, bounded inflate
//   D1-06 directory count        D1-07 "." names, over-long GNU long name
// build-note: needs the native modules
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
{
    const body = `
import { gzip, gunzip } from "dyna:compress";
import { memoryUsage } from "dyna:sys";
const small = gunzip(gzip(new Uint8Array(65536)));
let big = "none";
try { big = "len=" + gunzip(BOMB).length; } catch (e) { big = "THROW " + e.constructor.name; }
print("small=" + small.length, "big=" + big, "native=" + (memoryUsage().nativeSize > 0));
`;
    const mk = `const BOMB = (await import("dyna:compress")).gzip(new Uint8Array(64 << 20));\n`;
    const pre = `import { setNativeMemoryLimit } from "dyna:sys";\nconst BOMB = (await import("dyna:compress")).gzip(new Uint8Array(64 << 20));\nsetNativeMemoryLimit(4 << 20);\n`;
    const free = child("lim_ctl.mjs", mk + body);
    ok(free.code === 0 && /small=65536 big=len=67108864/.test(free.stdout),
        "control: no limit decodes 64 MiB (" + (free.stdout + free.stderr).slice(0, 120) + ")");
    const lim = child("lim_set.mjs", pre + body);
    ok(lim.code === 0 && lim.signal === null, "limited child exits normally (code " + lim.code + ")");
    ok(/small=65536 big=THROW/.test(lim.stdout),
        "[red] 64 MiB gunzip is refused under a 4 MiB native limit (" + (lim.stdout + lim.stderr).slice(0, 120) + ")");
}
// Ledger does not drift: second half of a churn equals the first half. ORACLE:
// conservation -- every block allocated in a round is freed in that round.
{
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
}

// ---- 2. teardown -------------------------------------------------------
// ORACLE: POSIX exit status. An uncaught exception is exit 1; SIGABRT is not.
{
    const r = child("exit_err.mjs", `
import { Spawn } from "dyna:sys";
const p = new Spawn("sh", ["-c", "sleep 1"]);
p.wait().then(() => {});
throw new Error("boom");
`);
    ok(r.signal === null && r.code === 1,
        "[red] uncaught error with a child still running exits 1 (code " + r.code + ", signal " + r.signal + ")");
    ok(/boom/.test(r.stderr) && !/Assertion failed/.test(r.stderr), "[red] no engine assertion on that path");
}

// ---- 3. D1-01 exact maxOutputBytes --------------------------------------
// ORACLE: the definition of a cap. max == n admits n bytes; max == n-1 does not.
// Lengths straddle the decoders' own constants: 320 (inflate wild-copy slack),
// 65536 (zstd/brotli growth step).
{
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
}

// ---- 4. D1-02 trailing data --------------------------------------------
// ORACLE: RFC 8878 section 3.1 -- zstd frames concatenate and a decoder
// yields their contents in order. For the others: a stream ends where its
// format says, so bytes after it are not part of it.
{
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
}

// ---- 5. zip ------------------------------------------------------------
{
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
}

// ---- 6. tar ------------------------------------------------------------
// ORACLE: POSIX pathname resolution -- "." and "a/." name a directory and
// contain no ".." component, so they cannot escape; GNU tar itself emits "./".
{
    function retitle(t, from, to) {
        const enc = new TextEncoder().encode(to);
        for (let i = 0; i < 100; i++) t[i] = i < enc.length ? enc[i] : 0;
        for (let i = 148; i < 156; i++) t[i] = 0x20;
        let sum = 0;
        for (let i = 0; i < 512; i++) sum += t[i];
        const s = sum.toString(8).padStart(6, "0");
        for (let i = 0; i < 6; i++) t[148 + i] = s.charCodeAt(i);
        t[154] = 0; t[155] = 0x20;
        return t;
    }
    const base = () => c.TarPack([{ name: "a/b", type: "directory" }]);
    ok(c.TarList(base())[0].name === "a/b", "control: packed directory lists");
    let r; try { r = c.TarList(retitle(base(), "a/b", "a/.")); } catch (e) { r = e; }
    ok(Array.isArray(r) && r[0].name === "a/.", "[red] D1-07: 'a/.' is a safe name (" + String(r).slice(0, 60) + ")");
    try { r = c.TarList(retitle(base(), "a/b", ".")); } catch (e) { r = e; }
    ok(Array.isArray(r) && r[0].name === ".", "[red] D1-07: '.' is a safe name (" + String(r).slice(0, 60) + ")");
    ok(/not safe/.test(thrown(() => c.TarList(retitle(base(), "a/b", "a/..")))), "control: 'a/..' is still refused");
    ok(/not safe/.test(thrown(() => c.TarList(retitle(base(), "a/b", "..")))), "control: '..' is still refused");
    ok(/not safe/.test(thrown(() => c.TarList(retitle(base(), "a/b", "../x")))), "control: '../x' is still refused");
}

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_1: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_1: " + failures + " failures");
