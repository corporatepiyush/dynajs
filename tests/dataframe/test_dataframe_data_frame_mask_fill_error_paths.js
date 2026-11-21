// covers: dyna:dataframe.DataFrame
// F38: MASK(mask, fill) converted a string fill before checking the mask and returned early on a bad or missing mask
// (and on mid-build failures) without freeing it (LSan: JS_ToCStringLen in dyn_df_mask, found by
// tests/api/test_api_fuzz.js). The fill is now owned by a wrapper that frees it on every path.
// Run under ASan (detect_leaks=1): before the fix each refused call leaks the fill string. Mutation: dropping the
// wrapper's free leaks on every call: red under ASan.
import { suite } from "../lib/kit.js";
import { DataFrame } from "dyna:dataframe";

const t = suite("dataframe.mask.fill_error_paths");
const df = new DataFrame({ a: new Float64Array([1, 2, 3, 4]), s: ["w", "x", "y", "z"] });

t.test("a refused mask with a string fill throws and the fill is released", (a) => {
    for (let i = 0; i < 32; i++) {
        const fill = "fill-" + i + "-" + "q".repeat(40 + i);
        a.throws(() => df.MASK(undefined, fill), TypeError, "missing mask");
        a.throws(() => df.MASK(new Uint8Array(3), fill), Error, "short mask");
    }
});

t.test("a good mask with a string fill still fills", (a) => {
    const fill = "f" + "-".repeat(30);
    const r = df.MASK(new Uint8Array([1, 0, 1, 0]), fill);
    a.eq(r.TO_COLUMNS().s.join(","), ["w", fill, "y", fill].join(","), "string column filled");
});

await t.run();
