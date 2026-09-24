// flags: --std
/* review probe: dyn_cmd_parse() no-arg path + the >65536 argv refusal and
 * its cleanup (leak hunt: the scriptArgs reference on that early return). */
import { Command } from "dyna:cli";
let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }

/* V1 explicit argv > 65536 refused with RangeError */
const huge = new Array(65537).fill("x");
let e = null;
try { new Command("t").parse(huge); } catch (x) { e = x; }
ok(e && e.constructor.name === "RangeError", "V1 65537 argv entries -> RangeError (got " +
   (e ? e.constructor.name : "no throw") + ")");
/* exactly 65536 is fine */
e = null;
try { new Command("t").parse(new Array(65536).fill("x")); } catch (x) { e = x; }
ok(e === null, "V2 65536 argv entries parse (got " + (e ? e.constructor.name + ": " + e.message : "ok") + ")");

/* V3 the no-arg path with a huge scriptArgs: same refusal, and the C code's
   early return must not leak the scriptArgs reference (ASan/teardown check
   sees it; here just exercise the path). */
const saved = scriptArgs;
let e3 = null, mode = "";
try {
    scriptArgs = new Array(70000).fill("y");
    mode = "writable";
    try { new Command("t").parse(); } catch (x) { e3 = x; }
} catch (x) {
    mode = "readonly:" + x.constructor.name;
}
try { scriptArgs = saved; } catch (x) {}
ok(mode !== "writable" || (e3 && e3.constructor.name === "RangeError"),
   "V3 no-arg parse with 70000 scriptArgs -> RangeError (mode=" + mode + " got " +
   (e3 ? e3.constructor.name : "no throw") + ")");
/* V4 no-arg parse with normal scriptArgs still works */
const r = new Command("t").parse();
ok(Array.isArray(r.arguments), "V4 no-arg parse returns arguments[] from scriptArgs");

print("probe_leak: " + (n - fails) + "/" + n + " ok");
if (fails) throw new Error("probe_leak failures: " + fails);
