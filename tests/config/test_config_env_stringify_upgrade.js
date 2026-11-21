// covers: dyna:config.Env.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env_stringify_upgrade");
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

__kitS.section("unquoted \\$PRE parses as literal", () => {
    const bare = Env.parse("A=\$PRE\nB=\"\$PRE\"\nC=x\$y");
    assert(bare.A === "$PRE", "unquoted \\$PRE parses as literal, got " +
        JSON.stringify(bare.A));
    assert(bare.B === "$PRE", "quoted \\$PRE parses as literal");
    assert(bare.C === "x$y", "\\y stays literal");

    let mi = 0;
    for (const assign of [true, false]) {
        for (const override of [false, true]) {
            for (const expand of [false, true]) {
                const k = "CG_M_KEY_" + mi, d = "CG_M_DOWN_" + mi;
                const f = new Path(String(ENV_FILE) + ".matrix").toString();
                writeFile(new Path(f),
                    k + "=from-file\n" + d + "=${" + k + "}-down\n");
                setEnv(k, "from-env");
                const rec = Env.load(f, { assign, override, expand });
                if (assign) {
                    assert(rec[k] === getEnv(k),
                        "record==env KEY: assign=" + assign + " override=" +
                        override + " expand=" + expand + " (rec " +
                        JSON.stringify(rec[k]) + " env " +
                        JSON.stringify(getEnv(k)) + ")");
                    assert(rec[d] === getEnv(d),
                        "record==env DOWN: assign=" + assign + " override=" +
                        override + " expand=" + expand + " (rec " +
                        JSON.stringify(rec[d]) + " env " +
                        JSON.stringify(getEnv(d)) + ")");
                    if (expand)
                        assert(rec[d] === (override ? "from-file-down"
                                                    : "from-env-down"),
                            "downstream expansion saw the winning value: " +
                            "override=" + override + " (rec " +
                            JSON.stringify(rec[d]) + ")");
                } else {
                    assert(rec[k] === "from-file",
                        "assign:false keeps the file value in the record");
                    assert(getEnv(k) === "from-env" && getEnv(d) === undefined,
                        "assign:false leaves the environment alone");
                    if (expand)
                        assert(rec[d] === "from-env-down",
                            "assign:false + expand still expands against env");
                }
                mi++;
                remove(new Path(f));
            }
        }
    }

    const escFile = new Path(String(ENV_FILE) + ".esc");
    writeFile(escFile, 'CG_E_Q="\\$CG_MISSING"\nCG_E_U=\\$CG_MISSING\n');
    const esc = Env.load(escFile.toString(), { expand: true, override: true });
    assert(esc.CG_E_Q === "$CG_MISSING",
        "quoted \\$X stays literal under expand, got " +
        JSON.stringify(esc.CG_E_Q));
    assert(esc.CG_E_U === "$CG_MISSING",
        "unquoted \\$X stays literal under expand");
    remove(escFile);
    setEnv("CG_M_KEY", "");
});

__kitS.section("has space", () => {
    const text = Env.stringify({ FOO: "bar", EMPTY: "", SP: "has space", NL: "a\nb" });
    assert(text === "FOO=bar\nEMPTY=\"\"\nSP=\"has space\"\nNL=\"a\\nb\"\n",
        "env stringify quotes what must be quoted");
    const back = Env.parse(text);
    assert(back.FOO === "bar" && back.EMPTY === "" && back.SP === "has space" &&
        back.NL === "a\nb", "env stringify/parse round-trips");
    throws(() => Env.stringify({ K: 42 }), TypeError,
        "env stringify refuses non-string values");
    throws(() => Env.stringify({ "": "v" }), TypeError,
        "env stringify refuses an empty key");
});
__kitS.end();

remove(ENV_FILE);
print("test_config_upgrade: all tests passed (" + n + " assertions)");
