// flags: --std
// timeout: 300
// covers: dyna:file.makeTempFile
// Split from tests/test_audit_w3_native_3.js: the file sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_make_temp_file_audit_w3_native_3");
import * as os from "os";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, makeTempFile, tempDir, writeFile, removeAll, remove } from "dyna:file";

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
// ---- P1-14 --------------------------------------------------------------
// ORACLE: POSIX. read(2) of a file that shrinks returns fewer bytes; touching
// a mapped page past the new end of file is SIGBUS. A reader may therefore
// see a short or empty result or an error, but must not die of a signal.
// The writer rewrites and truncates an 8 MiB file in a loop while the child
// reads it 300 times.

__kitS.section("control: a plain prefix lands directly in the temp dir", () => {
    const base = tempDir().toString().replace(/\/+$/, "");
    const f = makeTempFile("w3ok-");
    const fs = f.toString();
    ok(fs.startsWith(base + "/w3ok-") && fs.indexOf("/", base.length + 1) < 0, "control: a plain prefix lands directly in the temp dir (" + fs + ")");
    remove(f);
    for (const bad of ["../../w3esc-", "a/b", ".hidden", "x\\y"]) {
        let made = null, err = "none";
        try { made = makeTempFile(bad); } catch (e) { err = e.constructor.name; }
        if (made) { try { remove(made); } catch (e) {} }
        ok(err === "RangeError", "[red] P1-19: makeTempFile(" + JSON.stringify(bad) + ") is a RangeError (" + err + ")");
        err = "none"; made = null;
        try { made = makeTempDir(bad); } catch (e) { err = e.constructor.name; }
        if (made) { try { removeAll(made); } catch (e) {} }
        ok(err === "RangeError", "[red] P1-19: makeTempDir(" + JSON.stringify(bad) + ") is a RangeError (" + err + ")");
    }
});
__kitS.end();
try { removeAll(TMP); } catch (e) {}
print("test_file_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_file_audit_w3_native_3: " + failures + " failures");
