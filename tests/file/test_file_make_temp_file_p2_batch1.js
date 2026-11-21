// flags: --std
// covers: dyna:file.makeTempFile
// Split from tests/test_p2_batch1.js: the file sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_make_temp_file_p2_batch1");
import { File, Path, makeTempFile, makeTempDir, removeAll } from "dyna:file";
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); }
    }
}

__kitS.section("makeTempFile realpath round-trip", () => {
    const p = makeTempFile("p2-");
    const txt = "hi";
    new File(p).writeText(txt);
    eq(new File(p).readText(), txt, "makeTempFile realpath round-trip");
    const s = p.toString();
    assert(s.startsWith("/") && s.length > 5, "makeTempFile returns absolute path " + s);
    os.remove(s);
});
__kitS.end();
if (fails) { print("test_file_p2_batch1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_file_p2_batch1 failed"); }
print("test_file_p2_batch1: " + n + " assertions, 0 failures");
