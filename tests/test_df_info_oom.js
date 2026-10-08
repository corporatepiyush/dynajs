// flags: --memory-limit 32000000
// timeout: 120
import { DataFrame } from "dyna:dataframe";

const f = new DataFrame({ a: new Float64Array([1, 2, 3]), b: new Float64Array([4, 5, 6]) });
let hog = [];
try {
    for (let i = 0; i < 1000000; i++)
        hog.push(new Array(512).fill(1.5));
} catch (e) {}
let attempts = 0, ooms = 0;
for (let i = 0; i < 100000; i++) {
    try { hog.push(f.INFO()); attempts++; }
    catch (e) { ooms++; }
}
if (attempts + ooms !== 100000)
    throw new Error("INFO loop died early: " + attempts + " ok + " + ooms + " OOM");
print("test_df_info_oom: " + attempts + " INFO results / " + ooms +
      " OOM refusals under a tight limit, no double free");
