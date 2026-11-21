// covers: dyna:log.Debug
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_debug_extreme");
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, makeDir, writeFile, readFile, readDir,
         readLink, removeAll, exists } from "dyna:file";

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
function parseAll(lines) {
    const out = [];
    for (const l of lines) {
        let o = null;
        try { o = JSON.parse(l); } catch (e) { o = null; }
        assert(o !== null, "line parses: " + l.slice(0, 60));
        if (o) out.push(o);
    }
    return out;
}

const TMP = makeTempDir("dyna-log-extreme-");
const P = (s) => new Path(TMP + "/" + s);
for (const sub of ["best", "worst", "ugly", "torture"])
    makeDir(P(sub), { recursive: true });

function lines(name) {
    const body = readFile(P(name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

__kitS.section("Debug() no ns throws", () => {
    throws(() => Debug(), "Debug() no ns throws");
    throws(() => Debug(42), "Debug(42) throws");
    const d = Debug("x:y");
    eq(d(), undefined, "Debug() callable");
    eq(d("a"), undefined, "Debug(arg) callable");
    eq(d("a", "b", 3), undefined, "Debug multi-arg");
    const d2 = Debug("never:match");
    eq(d2("a"), undefined, "unmatched inert");
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_extreme: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_extreme failed");
}
print("test_log_extreme: " + n + " assertions, 0 failures");
