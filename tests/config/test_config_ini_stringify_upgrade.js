// covers: dyna:config.INI.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_ini_stringify_upgrade");
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

__kitS.section("has ; semicolon", () => {
    const text = INI.stringify({
        host: "localhost",
        port: 8080,
        debug: true,
        note: "plain",
        edge: "has ; semicolon",
        db: { user: "admin", pass: "p=1" },
    });
    assert(text.indexOf("host = localhost\n") === 0, "INI scalars emit first");
    assert(text.indexOf("\n[db]\nuser = admin\n") > 0, "sections emit as blocks");
    assert(text.indexOf('edge = "has ; semicolon"') > 0,
        "INI quotes values that cannot sit bare");
    const back = INI.parse(text);
    assert(back.host === "localhost", "bare strings round-trip");
    assert(back.edge === "has ; semicolon", "quoted INI values round-trip");
    assert(back.db.user === "admin", "sections round-trip");
    assert(back.port === "8080" && back.debug === "true",
        "numbers/booleans parse back as strings (INI has no numeric type)");

    throws(() => INI.stringify({ a: { b: { c: 1 } } }), TypeError,
        "INI refuses deeper nesting");
    throws(() => INI.stringify({ a: [1, 2] }), TypeError,
        "INI refuses array values");
    throws(() => INI.stringify({ a: null }), TypeError,
        "INI refuses null values");
});
__kitS.end();

remove(ENV_FILE);
print("test_config_upgrade: all tests passed (" + n + " assertions)");
