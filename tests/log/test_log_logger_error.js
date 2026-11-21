// covers: dyna:log.Logger.error
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_error");
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, readFile, readDir, readLink, removeAll } from "dyna:file";

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

__kitS.section("logging an Error", () => {
    const log = new Logger({ level: "trace", timestamp: false });
    eq(log.error(new Error("boom"), "failed"), undefined, "logging an Error");
    eq(log.error(new TypeError("bad type")), undefined, "an Error with no message arg");
    const e = new Error("outer");
    e.code = "ENOENT";
    eq(log.error(e, "with a code"), undefined, "an Error carrying extra fields");
});

const TMP = makeTempDir("dyna-log-test-");

function lines(name) {
    const body = readFile(new Path(TMP + "/" + name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

__kitS.section("err object leads", () => {
    const log = new Logger({ dest: TMP + "/errf.log", timestamp: false });
    const e = new Error("boom");
    e.code = "EPIPE";
    log.error(e, { reqId: "r1" }, "failed");
    log.flush();
    const [line] = lines("errf.log");
    assert(line.includes('"err":{"type":"Error","message":"boom"'), "err object leads");
    assert(line.includes('"code":"EPIPE"'), "error extra props ride along");
    assert(line.includes('"reqId":"r1"'), "the second object is fields");
    assert(line.includes('"msg":"failed"'), "the message is last");
});

__kitS.section("bad type", () => {
    const log = new Logger({ dest: TMP + "/texterr.log", format: "text",
                             timestamp: false });
    log.error(new TypeError("bad type"), "failed");
    log.flush();
    const [line] = lines("texterr.log");
    eq(line, "error failed TypeError: bad type", "error as `Type: message`");
});
__kitS.end();

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
