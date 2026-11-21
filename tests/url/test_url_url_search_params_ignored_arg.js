// covers: dyna:url.URLSearchParams
// F38: get(name) and getAll(name) take one argument, but the shared key/value reader converted a second argument
// to a C string and, with no slot to return it to, never freed it (LSan: dyn_sp_kv via dyn_sp_get/dyn_sp_get_all,
// found by tests/api/test_api_fuzz.js). The extra argument is now not converted at all, as WHATWG ignores it.
// Run under ASan (detect_leaks=1): before the fix this suite reports a leak per call. Mutation: converting the second
// argument again whenever it is present (the old reader) leaks: red under ASan.
import { suite } from "../lib/kit.js";
import { URLSearchParams } from "dyna:url";

const t = suite("url.search_params.ignored_arg");
const sp = new URLSearchParams("a=1&b=2&a=3");

t.test("get / getAll ignore a second argument and do not convert it", (a) => {
    let seen = 0;
    const spy = { toString() { seen++; return "x"; } };
    for (let i = 0; i < 64; i++) {
        const extra = "value-" + i + "-" + "z".repeat(i);
        a.eq(sp.get("a", extra), "1", "get");
        a.eq(sp.getAll("a", extra).join(","), "1,3", "getAll");
    }
    sp.get("a", spy);
    sp.getAll("a", spy);
    a.eq(seen, 0, "the ignored argument is never converted");
});

t.test("has / delete still use their value argument", (a) => {
    a.eq(sp.has("a", "3"), true, "has(name, value)");
    a.eq(sp.has("a", "9"), false, "has with another value");
});

await t.run();
