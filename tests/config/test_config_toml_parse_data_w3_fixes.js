// timeout: 180
// covers: dyna:config.TOML.parse
// Split from tests/test_data_w3_fixes.js: the config sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_toml_parse_data_w3_fixes");
import { TOML, Env } from "dyna:config";
import { Path, writeFile, remove, makeTempDir } from "dyna:file";
import { setEnv } from "dyna:sys";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}

await __kitS.section("s = \"\"\"a\"\"\"\"\"\"", async () => {
await throws(() => TOML.parse("s = \"\"\"a\"\"\"\"\"\""), "TOML multiline basic: a six-quote content run is refused");
});

await __kitS.section("s = '''a''''''", async () => {
await throws(() => TOML.parse("s = '''a''''''"), "TOML multiline literal: a six-apostrophe content run is refused");
});

await __kitS.section("x = [1, 2] [[x]]", async () => {
await throws(() => TOML.parse("x = [1, 2]\n[[x]]"), "TOML [[x]] over a static array is refused");
});

await __kitS.section("x = [{ a = 1 }] [[x]]", async () => {
await throws(() => TOML.parse("x = [{ a = 1 }]\n[[x]]"), "TOML [[x]] over a static inline array is refused");
});

await __kitS.section("x = [] [[x]]", async () => {
await throws(() => TOML.parse("x = []\n[[x]]"), "TOML [[x]] over an empty static array is refused");
});

__kitS.section("[[x]] a = 1 [[x]] a = 2", () => {
eq(TOML.parse("[[x]]\na = 1\n[[x]]\na = 2").x.length, 2, "TOML [[x]] still appends to a header array");
});

__kitS.section("x = [1, 2]", () => {
eq(TOML.parse("x = [1, 2]").x.length, 2, "a static array still parses");
});

__kitS.section("[[x]] [[x.y]] a = 1 [[x.y]] a = 2", () => {
eq(TOML.parse("[[x]]\n[[x.y]]\na = 1\n[[x.y]]\na = 2").x[0].y.length, 2,
   "a nested array-of-tables still parses");
if (fails === 0) print("test_config_data_w3_fixes: all " + n + " checks passed");
else print("test_config_data_w3_fixes: " + fails + " FAILED of " + n);
});
__kitS.end();
