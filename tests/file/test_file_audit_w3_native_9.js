// flags: --std
// Split from tests/test_audit_w3_native_9.js: the file sections.
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_file_audit_w3_native_9");
const best = (f, reps = 5) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = makeTempDir("w3n9_");
// ---- E2c-12: every loop shape is still interruptible ----------------------
// ORACLE: --timeout-ms ends a script with InternalError "interrupted" and
// status 1. Only a backward transfer can close a loop, so polling only taken
// backward branches must keep that true for every loop form; status 113 would
// be the hard-stop watchdog, meaning nothing in the loop looked at the clock.
// CONTROL: a script that finishes prints its result under the same flag.
t.test("E2c-12 every loop shape is interruptible", ({ ok }) => {
    const shapes = [
        "for(;;){}",
        "var i=0; do { i++; } while (i>=0 || i<0)",
        "var i=0; while (i < 1e12) { if (i & 1) i += 1; else i += 1; }",
        "var i=0; for(;;) { i = i ? 0 : 1; }",
        "var x=0; L: for(;;) { switch (x) { case 0: x=1; continue L; default: x=0; continue L; } }",
        "var a=[1]; for (var q of a) { a.push(1); }",
    ];
    let n = 0;
    for (const body of shapes) {
        const p = String(T) + "/loop" + (n++) + ".js";
        writeFile(new Path(p), body);
        const r = Exec(args()[0], ["--timeout-ms", "300", p], { timeoutMs: 20000, encoding: "utf8" });
        // The last shape grows an array without bound. Its soft interrupt lands
        // in time, but freeing that array on the way out can outlast the 250 ms
        // hard-stop grace on an instrumented build, and the watchdog then ends the
        // process itself (exit 113). Both are "stopped by the deadline" -- but
        // ONLY for the allocating shape: a pure CPU loop that needs the hard
        // stop has lost its interrupt check, which is what this case pins.
        const allocates = /push\(/.test(body);
        const soft = r.code === 1 && /interrupted/.test(r.stderr);
        ok(soft || (allocates && r.code === 113), "E2c-12: `" + body.slice(0, 40) + "` is interrupted by the deadline (code " + r.code + ")");
    }
    const p = String(T) + "/done.js";
    writeFile(new Path(p), "var s=0; for (var i=0;i<1000;i++){ if (i&1) s+=i; } print(s);");
    const r = Exec(args()[0], ["--timeout-ms", "5000", p], { timeoutMs: 20000, encoding: "utf8" });
    ok(r.code === 0 && r.stdout.trim() === "250000", "control: a finite loop finishes under the same flag (" + r.stdout.trim() + ")");
});
t.after(() => removeAll(new Path(String(T))));
await t.run();
