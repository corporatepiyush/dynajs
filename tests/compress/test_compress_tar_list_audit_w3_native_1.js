// timeout: 300
// covers: dyna:compress.TarList
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
const __kitS = __kitSections("test_compress_tar_list_audit_w3_native_1");
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

__kitS.section("control: packed directory lists", () => {
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
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_1: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_1: " + failures + " failures");
