// flags: --std
/* test_module_gating.js -- T3: `import "os"` must FAIL without --std (S1).
 * This file itself runs WITH --std (flags line) so the positive side holds;
 * the negative side is driven by the Exec child below. */
import { Exec } from "dyna:sys";
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };
import * as os from "os";
ok(typeof os.exec === "function", "with --std: os importable (positive)");
const child = `import * as os from "os"; console.log("LEAKED", typeof os.exec);`;
Exec("/bin/sh", ["-c",
  `printf '%s' > /tmp/gating_$$.js <<'JSEOF'\n${child}\nJSEOF\n./dynajs /tmp/gating_$$.js 2>&1 | head -2; rm -f /tmp/gating_$$.js`]);
const probe = Exec("/bin/sh", ["-c",
  `printf '%s' 'import * as os from "os"; print(typeof os.exec);' > /tmp/gprobe.js && ./dynajs /tmp/gprobe.js 2>&1 | head -2; rm -f /tmp/gprobe.js`]);
ok(!probe.stdout.includes("function"), "without --std: os.exec unreachable");
ok(probe.stderr.includes("could not load module") || probe.stdout.includes("could not load") || /Error/.test(probe.stdout + probe.stderr),
   "module load error surfaced: " + (probe.stdout + probe.stderr).slice(0, 60));
print("test_module_gating: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_module_gating: " + fail + " failures");
