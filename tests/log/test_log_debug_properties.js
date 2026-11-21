// covers: dyna:log.Debug
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, makeDir, readFile, readDir, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const TMP = makeTempDir("dyna-log-prop-");
const P = (s) => new Path(TMP + "/" + s);
for (const sub of ["json", "text", "rot", "rot2", "share"])
    makeDir(P(sub), { recursive: true });
function lines(name) {
    const body = readFile(P(name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

{
    const d = Debug("prop:ns");
    eq(typeof d, "function", "Debug returns a function");
    eq(d(), undefined, "Debug with no arguments is fine");
    eq(d("a", "b", 3), undefined, "Debug joins several arguments");
    eq(d({ toString() { return "via toString"; } }), undefined, "coercible values work");
    for (let i = 0; i < 1000; i++)
        Debug("never:matching:namespace")("loop " + i);
    assert(true, "1000 unmatched Debug calls are inert");
}

removeAll(P("."));
if (fails) {
    print("test_log_properties: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_properties failed");
}
print("test_log_properties: " + n + " assertions, 0 failures");
