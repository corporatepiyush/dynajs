// timeout: 120
// flags: --std
// Split from tests/test_platform_hardening.js: the cli sections.
import { Table, Command } from "dyna:cli";
import { Path, stat } from "dyna:file";
import { chDir } from "dyna:sys";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throwsRange(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof RangeError; }
    assert(t, msg);
}
function throwsType(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof TypeError; }
    assert(t, msg);
}
{
    const E = "a\u001b[2Jb";
    let leaked = 0;
    const check = (fn, what) => {
        let m = "";
        try { fn(); } catch (e) { m = String(e.message || e); }
        assert(m.length > 0, what + " throws");
        if (m.indexOf("\u001b") >= 0) leaked++;
        assert(m.indexOf("\\x1b") >= 0, what + " escapes the control byte");
    };
    check(() => Table([["x"]], { format: E }), "Table format");
    check(() => Table([["x"]], { align: [E] }), "Table align");
    check(() => { const c = new Command("t"); c.option("--" + E); c.option("--" + E); },
        "Command duplicate option");
    check(() => stat(new Path("/definitely-missing-" + E)), "file.stat");
    check(() => chDir(new Path("/definitely-missing-" + E)), "sys.chDir");
    assert(leaked === 0, "no raw control bytes in any diagnostic");
}
print("test_cli_platform_hardening: all tests passed (" + n + " assertions)");
