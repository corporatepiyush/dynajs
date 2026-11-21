// covers: dyna:sys.Which
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_which_proc");
import { Exec, Which, setEnv, getEnv } from "dyna:sys";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

__kitS.section("Which finds sh", () => {
    const sh = Which("sh");
    assert(typeof sh === "string" && sh.indexOf("/sh") > 0, "Which finds sh (" + sh + ")");
    eq(Exec(sh, ["-c", "echo via-which"]).stdout, "via-which\n",
       "and what it returns is runnable");
});
__kitS.end();

eq(Which("definitely-not-a-real-command-xyz"), null, "a missing command is null");

eq(Which(""), null, "an empty name is null, not the current directory");

eq(Which("/bin/sh"), "/bin/sh", "an absolute path is checked, not searched");

eq(Which("/definitely/not/here"), null, "and a bad one is null");

eq(Which("/etc"), null, "a directory is not an executable");

throws(() => Which(), "a name is required");

throws(() => Which(42), "and it must be a string");

if (fails) {
    print("test_proc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_proc failed");
}
print("test_proc: " + n + " assertions, 0 failures");
