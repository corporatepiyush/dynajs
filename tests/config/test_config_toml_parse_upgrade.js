// covers: dyna:config.TOML.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_toml_parse_upgrade");
import { Env, INI, TOML } from "dyna:config";
import { getEnv, setEnv } from "dyna:sys";
import { Path, writeFile, remove } from "dyna:file";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor,
        msg + " (" + ctor.name + ", got " + caught + ")");
}

const ENV_FILE = new Path((getEnv("TMPDIR") || "/tmp") + "/cg_test_upgrade." + Date.now() + "." + Math.random().toString(36).slice(2) + ".env");
writeFile(ENV_FILE,
    "CG_T_BASE=hello\n" +
    "CG_T_DERIVED=${CG_T_BASE}-world\n" +
    "CG_T_EXISTING=from-file\n" +
    "CG_T_QUOTED=\"a # b\"\n" +
    "export CG_T_EXPORTED=yes\n");

__kitS.section("when = 1979-05-27T07:32:00Z", () => {
    const dt = TOML.parse('when = 1979-05-27T07:32:00Z\n');
    assert(dt.when === "1979-05-27T07:32:00Z",
        "offset date-time parses as the raw STRING, got " + JSON.stringify(dt.when));
    const local = TOML.parse('d = 1979-05-27\nt = 07:32:00\n');
    assert(local.d === "1979-05-27" && local.t === "07:32:00",
        "local date and time parse as strings too");
});
__kitS.end();

remove(ENV_FILE);
print("test_config_upgrade: all tests passed (" + n + " assertions)");
