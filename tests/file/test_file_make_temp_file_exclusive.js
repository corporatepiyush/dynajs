// covers: dyna:file.makeTempFile
// tests/file/test_file_make_temp_file_exclusive.js -- makeTempFile is mkstemp: the file
// is created with O_CREAT|O_EXCL and mode 0600, so it is private to the user
// and a name that already exists is never opened, truncated or returned again
// (SEC-8, B17 ruling NOT-A-BUG: the Path API stays; this pins the guarantee).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_make_temp_file_exclusive");
import { makeTempFile, stat, readFile, writeFile, remove } from "dyna:file";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

__kitS.section("has mode 0600, got 0", () => {
const made = [];
try {
    for (let i = 0; i < 64; i++) {
        const p = makeTempFile("excl-");
        const st = stat(p);
        check((st.mode & 0o777) === 0o600, p + " has mode 0600, got 0" + (st.mode & 0o777).toString(8));
        check(st.size === 0, p + " starts empty");
        writeFile(p, "owner-" + i);
        made.push(p);
    }
    const names = new Set(made.map(String));
    check(names.size === made.length, "64 calls returned 64 distinct names");
    made.forEach((p, i) => check(readFile(p) === "owner-" + i, p + " kept its content: no later call reused or truncated it"));
} finally {
    for (const p of made) { try { remove(p); } catch (e) { } }
}
});
__kitS.end();

print("test_file_temp_exclusive: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_file_temp_exclusive: " + failures + " failures");
