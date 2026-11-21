// timeout: 300
// covers: integration c:src/dyna-cli.c c:src/modules/crypto/crypto-argon2.c c:src/modules/crypto/crypto-kdf.c c:src/modules/html/html-template.c -- --native-memory-limit and the work budget refuse oversize argon2id, scrypt and template renders in a child
// Split from tests/test_audit_w3_native_3.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_audit_w3_native_3");
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
async function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const TMP = makeTempDir("w3n3-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: name === "budget2.mjs" ? 8000 : 60000, encoding: "utf8" });
}
// ---- D3-10 / D2-05 / D3-12 / D2-12 --------------------------------------
// ORACLE: arithmetic against a 32 MiB native budget. Argon2id with m = 512 MiB
// and a template that renders 300 MB cannot fit; their small forms do.
// Scrypt: RFC 7914 cost is N*r*p block mixes; N=2^19, r=8, p=32 is 2^27,
// above the 2^26 work budget while its memory (512 MiB) is inside the 1 GiB
// memory budget, so only the work bound can refuse it. The child is given
// 8 s: a build without the bound computes for minutes and is cut off.
await __kitS.section("D3-10 / D2-05 / D3-12 / D2-12", async () => {
    const r = child("budget.mjs", `
import { Argon2id, Scrypt } from "dyna:crypto";
import { Template } from "dyna:html";
const t = async (n, f) => { try { const v = await f(); print(n + "=ok:" + (v && v.length)); } catch (e) { print(n + "=" + e.constructor.name + ":" + String(e.message).slice(0, 60)); } };
await t("argonSmall", () => Argon2id.hash("pw", new Uint8Array(16), { memory: 1024, iterations: 1, encoded: false }));
await t("argonBig", () => Argon2id.hash("pw", new Uint8Array(16), { memory: 524288, iterations: 1, encoded: false }));
await t("tplSmall", () => new Template("{{#a}}x{{/a}}").render({ a: [1, 2, 3] }));
await t("tplBig", () => new Template("{{#a}}" + "x".repeat(1000) + "{{/a}}").render({ a: new Array(300000).fill(1) }));
await t("tplTags", () => new Template("{{a}}".repeat(70000)));
await t("scryptSmall", () => Scrypt("pw", new Uint8Array(16), { N: 1024, r: 8, p: 16, keyLen: 64 }));
`, ["--native-memory-limit", "33554432"]);
    const w = child("budget2.mjs", `
import { Scrypt } from "dyna:crypto";
try { await Scrypt("pw", new Uint8Array(16), { N: 1 << 19, r: 8, p: 32, keyLen: 32 }); print("scryptWork=ok"); }
catch (e) { print("scryptWork=" + e.constructor.name + ":" + String(e.message).slice(0, 60)); }
`);
    const o = r.stdout;
    ok(/argonSmall=ok:32/.test(o) && /tplSmall=ok:3/.test(o) && /scryptSmall=ok:64/.test(o), "control: small Argon2id / Template / Scrypt run under a 32 MiB native limit (" + (o + r.stderr).slice(0, 140) + ")");
    ok(/argonBig=InternalError:out of memory/.test(o), "D3-10: Argon2id m=512 MiB is refused under the native limit");
    ok(/tplBig=InternalError:out of memory/.test(o), "D2-05: a 300 MB render is refused under the native limit");
    ok(/tplTags=SyntaxError:.*more than 65536/.test(o), "[red] D2-12: the tag cap is named, not reported as out of memory (" + (/tplTags=[^\n]*/.exec(o) || [""])[0] + ")");
    ok(/scryptWork=RangeError:.*work budget/.test(w.stdout), "[red] D3-12: Scrypt N*r*p above 2^26 is refused (" + (w.stdout.trim() || "no row: child cut off after 8 s") + ")");
});
__kitS.end();
try { removeAll(TMP); } catch (e) {}
print("test_core_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_core_audit_w3_native_3: " + failures + " failures");
