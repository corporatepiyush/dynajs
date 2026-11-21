// covers: dyna:log.Logger.flush
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_flush_extreme");
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

__kitS.section("buffer:0 writes straight", () => {
    const L0 = new Logger({ dest: P("ugly/buf0.log").toString(), timestamp: false,
                            buffer: 0 });
    L0.info("immediate");
    L0.flush();
    eq(lines("ugly/buf0.log").length, 1, "buffer:0 writes straight");

    const Lf = new Logger({ dest: P("ugly/buffalse.log").toString(), timestamp: false,
                            buffer: false });
    Lf.info("immediate2");
    Lf.flush();
    eq(lines("ugly/buffalse.log").length, 1, "buffer:false");

    const L1 = new Logger({ dest: P("ugly/buf1.log").toString(), timestamp: false,
                            buffer: 1 });
    L1.info("x");
    eq(lines("ugly/buf1.log").length, 1, "buffer:1 bypasses (line > buffer)");
    L1.flush();
    eq(lines("ugly/buf1.log").length, 1, "flush lands nothing new");

    throws(() => new Logger({ dest: P("ugly/bb.log").toString(), buffer: 2097152 }),
           "buffer over 1 MiB throws");
    throws(() => new Logger({ dest: P("ugly/bb.log").toString(), buffer: -1 }),
           "negative buffer throws");
    const b15 = new Logger({ dest: P("ugly/bb15.log").toString(), timestamp: false,
                             buffer: 1.5 });
    b15.flush();
});

__kitS.section("size 0 throws", () => {
    throws(() => new Logger({ dest: P("ugly/r0.log").toString(), rollover: { size: 0 } }),
           "size 0 throws");
    throws(() => new Logger({ dest: P("ugly/r0.log").toString(), rollover: { size: "" } }),
           "empty size throws");
    throws(() => new Logger({ dest: P("ugly/r0.log").toString(), rollover: { size: "m" } }),
           "unit-only throws");
    throws(() => new Logger({ dest: P("ugly/r0.log").toString(),
                              rollover: { frequency: "weekly" } }), "weekly throws");
    throws(() => new Logger({ dest: P("ugly/r0.log").toString(),
                              rollover: { count: -1 } }), "count -1 throws");
    const c15 = new Logger({ dest: P("ugly/rc15.log").toString(), timestamp: false,
                             rollover: { count: 1.5 } });
    c15.flush();
    throws(() => new Logger({ dest: P("ugly/r0.log").toString(),
                              rollover: { size: "1x" } }), "unit x throws");
    const both = new Logger({ dest: P("ugly/rboth.log").toString(), mkdir: true,
                              timestamp: false,
                              rollover: { size: 50, frequency: 3600000 } });
    for (let i = 0; i < 5; i++) both.info("padding padding padding");
    both.flush();
    const fs = readDir(P("ugly")).map((e) => e.name).filter((f) => /^rboth\.\d+\.log$/.test(f));
    assert(fs.length >= 2, "size+frequency both rotate: " + fs.length);
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_extreme: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_extreme failed");
}
print("test_log_extreme: " + n + " assertions, 0 failures");
