// covers: dyna:config.Env.load
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env_load_upgrade");
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

__kitS.section("load returns the parsed record", () => {
    const envPath = ENV_FILE.toString();
    const rec = Env.load(envPath);
    assert(rec.CG_T_BASE === "hello", "load returns the parsed record");
    assert(getEnv("CG_T_BASE") === "hello", "load assigns into the environment");
    assert(getEnv("CG_T_EXPORTED") === "yes", "export prefix handled by load");

    setEnv("CG_T_EXISTING", "original");
    Env.load(envPath);
    assert(getEnv("CG_T_EXISTING") === "original",
        "override defaults to false (existing entries win)");
    Env.load(envPath, { override: true });
    assert(getEnv("CG_T_EXISTING") === "from-file",
        "override: true replaces existing entries");

    setEnv("CG_T_OUTER", "outer");
    const ex = Env.load(envPath, { expand: true, override: true });
    assert(ex.CG_T_DERIVED === "hello-world",
        "expand resolves earlier keys of the same file");
    assert(getEnv("CG_T_DERIVED") === "hello-world",
        "expanded value is what gets assigned");

    setEnv("CG_T_BASE", "untouched");
    const parsed = Env.load(envPath, { assign: false });
    assert(parsed.CG_T_BASE === "hello", "assign:false still returns the record");
    assert(getEnv("CG_T_BASE") === "untouched",
        "assign:false leaves the environment alone");

    throws(() => Env.load("cg_test_no_such.env"), Error, "missing file throws");
    throws(() => Env.load(envPath, { assign: "yes" }), TypeError,
        "non-boolean assign refused (strict bag)");
    throws(() => Env.load(envPath, { expand: 1 }), TypeError,
        "non-boolean expand refused");

    let caught = null;
    try { Env.load(envPath, { overide: true }); } catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "overide" (valid: assign, override, expand)',
        "unknown key refused naming key AND valid set, got: " + caught);
    const recFull = Env.load(envPath, { assign: false, override: true, expand: true });
    assert(recFull.CG_T_BASE !== undefined, "the full valid bag is unchanged");
    throws(() => Env.load(envPath, 42), TypeError,
        "a primitive bag is still refused outright");
});
__kitS.end();

remove(ENV_FILE);
print("test_config_upgrade: all tests passed (" + n + " assertions)");
