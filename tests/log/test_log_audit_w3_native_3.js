// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_3.js: the log sections.
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { Logger } from "dyna:log";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const TMP = makeTempDir("w3n3-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: name === "budget2.mjs" ? 8000 : 60000, encoding: "utf8" });
}
// ---- P1-17 --------------------------------------------------------------
// ORACLE: ECMA-48. U+009B is the 8-bit CSI, U+007F is DEL; with C0 already
// escaped these are the remaining ways a logged value drives a terminal.
// U+2028 is a line terminator for line-oriented consumers.
// CONTROL: printable non-ASCII text (U+00E9, U+4E2D) passes through unescaped.
{
    const lines = [];
    const L = new Logger({ format: "text", timestamp: false, dest: (l) => lines.push(l) });
    L.info("a\u009b31mb del\u007f sep end café 中");
    const out = lines.join("");
    ok(out.length > 0, "fixture: the text logger produced a line");
    ok(!out.includes("\u009b"), "[red] P1-17: no raw U+009B in a text line");
    ok(!out.includes("\u007f"), "[red] P1-17: no raw DEL in a text line");
    ok(!out.includes(" "), "[red] P1-17: no raw U+2028 in a text line");
    ok(out.includes("\\u009b") && out.includes("\\u007f") && out.includes("\\u2028"), "the three are visible as escapes (" + JSON.stringify(out).slice(0, 120) + ")");
    ok(out.includes("café 中"), "control: printable non-ASCII text is untouched");
}
try { removeAll(TMP); } catch (e) {}
print("test_log_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_log_audit_w3_native_3: " + failures + " failures");
