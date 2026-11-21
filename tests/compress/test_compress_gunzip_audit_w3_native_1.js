// timeout: 300
// covers: dyna:compress.gunzip
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
const __kitS = __kitSections("test_compress_gunzip_audit_w3_native_1");
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

await __kitS.section("import { gzip, gunzip } from \"dyna:compress\"; import { memoryUsage } from \"dyna:sys\"", async () => {
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
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_1: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_1: " + failures + " failures");
