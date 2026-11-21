// timeout: 180
// covers: dyna:config.Env.load
// Split from tests/test_data_w3_fixes.js: the config sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env_load_data_w3_fixes");
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

const envPath = new Path(makeTempDir("w3env"), "w3_env_order.env");

writeFile(envPath, "981234568=b\n981234567=$981234568\n");

__kitS.section("COMPAT-109: expansion follows file order for integer-like keys", () => {
    const rec = Env.load(envPath.toString(), { expand: true, override: true });
    eq(rec["981234567"], "b",
       "COMPAT-109: expansion follows file order for integer-like keys");
});

setEnv("981234567", "");
setEnv("981234568", "");
writeFile(envPath, "W3QUOTED=\"a\\nb\"\n");
__kitS.section("COMPAT-110: quoted escapes survive expand:true", () => {
    const rec = Env.load(envPath.toString(), { expand: true, assign: false });
    eq(rec.W3QUOTED, "a\nb", "COMPAT-110: quoted escapes survive expand:true");
});

writeFile(envPath, "W3Q2=\" x $W3NOPE y \"\n");
__kitS.section("x y", () => {
    const rec = Env.load(envPath.toString(), { expand: true, assign: false });
    eq(rec.W3Q2, " x  y ", "COMPAT-110: expansion still runs inside a quoted value");
    remove(envPath);
});
__kitS.end();
if (fails === 0) print("test_config_data_w3_fixes: all " + n + " checks passed");
else print("test_config_data_w3_fixes: " + fails + " FAILED of " + n);
