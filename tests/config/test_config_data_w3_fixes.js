// timeout: 180
// Split from tests/test_data_w3_fixes.js: the config sections.
import { TOML, Env } from "dyna:config";
import { Path, writeFile, remove } from "dyna:file";
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
await throws(() => TOML.parse("s = \"\"\"a\"\"\"\"\"\""), "TOML multiline basic: a six-quote content run is refused");
await throws(() => TOML.parse("s = '''a''''''"), "TOML multiline literal: a six-apostrophe content run is refused");
await throws(() => TOML.parse("x = [1, 2]\n[[x]]"), "TOML [[x]] over a static array is refused");
await throws(() => TOML.parse("x = [{ a = 1 }]\n[[x]]"), "TOML [[x]] over a static inline array is refused");
await throws(() => TOML.parse("x = []\n[[x]]"), "TOML [[x]] over an empty static array is refused");
eq(TOML.parse("[[x]]\na = 1\n[[x]]\na = 2").x.length, 2, "TOML [[x]] still appends to a header array");
eq(TOML.parse("x = [1, 2]").x.length, 2, "a static array still parses");
eq(TOML.parse("[[x]]\n[[x.y]]\na = 1\n[[x.y]]\na = 2").x[0].y.length, 2,
   "a nested array-of-tables still parses");
const envPath = Path.cwd().join("w3_env_order.env");
writeFile(envPath, "981234568=b\n981234567=$981234568\n");
{
    const rec = Env.load(envPath.toString(), { expand: true, override: true });
    eq(rec["981234567"], "b",
       "COMPAT-109: expansion follows file order for integer-like keys");
}
setEnv("981234567", "");
setEnv("981234568", "");
writeFile(envPath, "W3QUOTED=\"a\\nb\"\n");
{
    const rec = Env.load(envPath.toString(), { expand: true, assign: false });
    eq(rec.W3QUOTED, "a\nb", "COMPAT-110: quoted escapes survive expand:true");
}
writeFile(envPath, "W3Q2=\" x $W3NOPE y \"\n");
{
    const rec = Env.load(envPath.toString(), { expand: true, assign: false });
    eq(rec.W3Q2, " x  y ", "COMPAT-110: expansion still runs inside a quoted value");
}
remove(envPath);
if (fails === 0) print("test_config_data_w3_fixes: all " + n + " checks passed");
else print("test_config_data_w3_fixes: " + fails + " FAILED of " + n);
