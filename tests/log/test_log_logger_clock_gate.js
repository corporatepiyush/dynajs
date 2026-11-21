// covers: c:src/modules/log/log-format.c
// An emit reads the wall clock only when something uses it: a timestamp ("epoch" or "iso"), or a
// file destination with size or frequency rollover (the rotation check takes the time). With
// timestamp: false and no rollover the clock_gettime call (2-7% of Logger#info) is skipped.
// Checked here: epoch and iso lines carry the current time (bracketed by Date.now() around the
// call), timestamp: false lines carry none, and a frequency rollover with timestamp: false still
// rotates once its period has passed, as does a size rollover.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_clock_gate");
import { Logger } from "dyna:log";
import { makeTempDir, readDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL " + m); } };
const spin = (ms) => { const t = Date.now() + ms; while (Date.now() < t); };

__kitS.section("timestamps are current when asked for, absent otherwise", () => {
    const lines = [];
    const ep = new Logger({ timestamp: "epoch", dest: (x) => lines.push(x) });
    const iso = new Logger({ timestamp: "iso", dest: (x) => lines.push(x) });
    const none = new Logger({ timestamp: false, dest: (x) => lines.push(x) });
    const txt = new Logger({ timestamp: "epoch", format: "text", dest: (x) => lines.push(x) });
    for (let i = 0; i < 50; i++) {
        const t0 = Date.now();
        ep.info("e");
        iso.info("i");
        none.info("n");
        txt.info("t");
        const t1 = Date.now();
        const [e, s, n, t] = lines.splice(0, 4);
        const et = JSON.parse(e).time;
        ok(typeof et === "number" && et >= t0 && et <= t1, "epoch time " + et + " in [" + t0 + ", " + t1 + "]");
        const it = Date.parse(JSON.parse(s).time);
        ok(it >= t0 && it <= t1, "iso time " + JSON.parse(s).time + " in range");
        ok(!("time" in JSON.parse(n)), "no time without a timestamp: " + n.trim());
        const tt = +t.split(" ")[0];
        ok(tt >= t0 && tt <= t1, "text epoch time " + tt + " in range");
    }
});

__kitS.section("rollover rotates without a timestamp", () => {
    const dirP = makeTempDir("dyna-log-clock-"), dir = String(dirP);
    const names = (p) => readDir(dirP).map((e) => e.name).filter((f) => f.startsWith(p));
    try {
        const fq = new Logger({ timestamp: false, dest: dir + "/fq.log", rollover: { frequency: 60 } });
        fq.info("first");
        spin(150);
        fq.info("second");
        fq.flush();
        const fqFiles = names("fq");
        ok(fqFiles.length >= 2, "frequency rollover made " + fqFiles.length + " files: " + fqFiles.join(","));
        const sz = new Logger({ timestamp: false, dest: dir + "/sz.log", rollover: { size: 100 } });
        for (let i = 0; i < 10; i++) sz.info("line " + i + " " + "p".repeat(40));
        sz.flush();
        const szFiles = names("sz");
        ok(szFiles.length >= 3, "size rollover made " + szFiles.length + " files");
    } finally {
        removeAll(dirP);
    }
});

__kitS.end();
print(`test_log_logger_clock_gate: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
