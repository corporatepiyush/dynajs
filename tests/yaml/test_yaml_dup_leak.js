// flags: --std
// A duplicate mapping key is checked with JS_GetOwnProperty. The flow-mapping
// check and the key-by-key path (a block mapping past 16 keys) asked for the
// whole descriptor, which holds a counted reference to the first value, and
// dropped it on the refusal path: every refused document leaked that value.
// The instrument is the allocator's own block count.
import * as std from "std";
import * as Y from "dyna:yaml";
import { memoryUsage } from "dyna:sys";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

function churn(text) {
    let refused = 0;
    const once = () => { try { Y.Parse(text); } catch (e) { if (/duplicate mapping key/.test(e.message)) refused++; } };
    once();
    std.gc();
    const m0 = memoryUsage().mallocCount;
    for (let i = 0; i < 2000; i++) once();
    std.gc();
    return { grew: memoryUsage().mallocCount - m0, refused };
}

const long = (k) => "value-that-is-a-heap-string-" + k;
const flow = churn("{a: " + long("a") + ", b: 1, a: " + long("b") + "}\n");
ok(flow.refused === 2001, "the flow mapping is refused every time (" + flow.refused + ")");
ok(flow.grew < 4, "a refused flow mapping frees the first value (grew " + flow.grew + " blocks over 2000)");

let block = "";
for (let i = 0; i < 20; i++) block += "k" + i + ": " + long(i) + "\n";
block += "k17: " + long("again") + "\n";
const spill = churn(block);
ok(spill.refused === 2001, "the block mapping past 16 keys is refused every time (" + spill.refused + ")");
ok(spill.grew < 4, "a refused long block mapping frees the first value (grew " + spill.grew + " blocks over 2000)");

print("test_yaml_dup_leak: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failed");
