/* test_config_upgrade.js -- phase-1 additions to dyna:config.
 *
 * Env.load(path, {assign, override, expand}) -- parse a .env file AND
 *   inject it into the process environment via setenv(3). Defaults are
 *   dotenv's: assign ON, override OFF (existing entries win), expand OFF.
 *   Returns the record the file parses to (the assigned values when
 *   assignment happens; values expanded when expand is on).
 * Env.stringify(record) -> .env text; INI.stringify(record) -> INI
 *   text (scalars, then [section] blocks). Strict: values must be strings
 *   (Env) / strings, numbers or booleans (INI); nested objects beyond one
 *   section level are refused.
 * TOML.parse returns date-time values as STRINGS (the raw RFC 3339
 *   form). No {dates} option: local date-times carry no offset, so a unix
 *   conversion would be ambiguous -- documented, not implemented.
 *
 * HERMETIC: Env.load writes only into THIS process's environment, using a
 * CG_TEST_ key prefix so nothing real is clobbered.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_config_upgrade.js
 */

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

const ENV_FILE = Path.cwd().join("cg_test_upgrade.env");
writeFile(ENV_FILE,
    "CG_T_BASE=hello\n" +
    "CG_T_DERIVED=${CG_T_BASE}-world\n" +
    "CG_T_EXISTING=from-file\n" +
    "CG_T_QUOTED=\"a # b\"\n" +
    "export CG_T_EXPORTED=yes\n");

/* ----------------: Env.load ---------------- */
{
    const envPath = ENV_FILE.toString();     /* Env.load takes a plain path */
    const rec = Env.load(envPath);
    assert(rec.CG_T_BASE === "hello", "load returns the parsed record");
    assert(getEnv("CG_T_BASE") === "hello", "load assigns into the environment");
    assert(getEnv("CG_T_EXPORTED") === "yes", "export prefix handled by load");

    /* override defaults OFF: an existing entry wins */
    setEnv("CG_T_EXISTING", "original");
    Env.load(envPath);
    assert(getEnv("CG_T_EXISTING") === "original",
        "override defaults to false (existing entries win)");
    Env.load(envPath, { override: true });
    assert(getEnv("CG_T_EXISTING") === "from-file",
        "override: true replaces existing entries");

    /* expansion is opt-in and sees the environment + earlier keys */
    setEnv("CG_T_OUTER", "outer");
    const ex = Env.load(envPath, { expand: true, override: true });
    assert(ex.CG_T_DERIVED === "hello-world",
        "expand resolves earlier keys of the same file");
    assert(getEnv("CG_T_DERIVED") === "hello-world",
        "expanded value is what gets assigned");

    /* assign: false = parse only */
    setEnv("CG_T_BASE", "untouched");
    const parsed = Env.load(envPath, { assign: false });
    assert(parsed.CG_T_BASE === "hello", "assign:false still returns the record");
    assert(getEnv("CG_T_BASE") === "untouched",
        "assign:false leaves the environment alone");

    /* errors and strictness */
    throws(() => Env.load("cg_test_no_such.env"), Error, "missing file throws");
    throws(() => Env.load(envPath, { assign: "yes" }), TypeError,
        "non-boolean assign refused (strict bag)");
    throws(() => Env.load(envPath, { expand: 1 }), TypeError,
        "non-boolean expand refused");

    /*the key set is closed too (unknown key, exact message) */
    let caught = null;
    try { Env.load(envPath, { overide: true }); } catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "overide" (valid: assign, override, expand)',
        "unknown key refused naming key AND valid set, got: " + caught);
    const recFull = Env.load(envPath, { assign: false, override: true, expand: true });
    assert(recFull.CG_T_BASE !== undefined, "the full valid bag is unchanged");
    throws(() => Env.load(envPath, 42), TypeError,
        "a primitive bag is still refused outright");
}

/* ---------------- review fixes: \$ escape + record==environment ------- */
{
    /* the escape is BACKSLASH-dollar in both quotings, and a quoted "\$X"
     * must not turn into a live reference at parse time */
    const bare = Env.parse("A=\$PRE\nB=\"\$PRE\"\nC=x\$y");
    assert(bare.A === "$PRE", "unquoted \\$PRE parses as literal, got " +
        JSON.stringify(bare.A));
    assert(bare.B === "$PRE", "quoted \\$PRE parses as literal");
    assert(bare.C === "x$y", "\\y stays literal");

    /* record==environment matrix over {assign, override, expand} with a
     * pre-existing key */
    /* fresh key names per iteration: the environment cannot unset entries,
     * and a leftover CG key would win under override:false and mask the
     * behavior under test */
    let mi = 0;
    for (const assign of [true, false]) {
        for (const override of [false, true]) {
            for (const expand of [false, true]) {
                const k = "CG_M_KEY_" + mi, d = "CG_M_DOWN_" + mi;
                const f = Path.cwd().join("cg_fix_matrix.env").toString();
                writeFile(new Path(f),
                    k + "=from-file\n" + d + "=${" + k + "}-down\n");
                setEnv(k, "from-env");
                const rec = Env.load(f, { assign, override, expand });
                if (assign) {
                    /* the fix under test: the record IS the environment's
                     * view, key by key, including the key the env won */
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
                    /* assign:false is parse-only: the record holds the FILE
                     * values and the environment is untouched */
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

    /* quoted "\$X" survives expand:true as a literal */
    const escFile = Path.cwd().join("cg_fix_esc.env");
    writeFile(escFile, 'CG_E_Q="\\$CG_MISSING"\nCG_E_U=\\$CG_MISSING\n');
    const esc = Env.load(escFile.toString(), { expand: true, override: true });
    assert(esc.CG_E_Q === "$CG_MISSING",
        "quoted \\$X stays literal under expand, got " +
        JSON.stringify(esc.CG_E_Q));
    assert(esc.CG_E_U === "$CG_MISSING",
        "unquoted \\$X stays literal under expand");
    remove(escFile);
    setEnv("CG_M_KEY", "");
}

/* ----------------: Env.stringify ---------------- */
{
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
}

/* ----------------: INI.stringify ---------------- */
{
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
}

/* ----------------: TOML datetime probe ---------------- */
{
    const dt = TOML.parse('when = 1979-05-27T07:32:00Z\n');
    assert(dt.when === "1979-05-27T07:32:00Z",
        "offset date-time parses as the raw STRING, got " + JSON.stringify(dt.when));
    const local = TOML.parse('d = 1979-05-27\nt = 07:32:00\n');
    assert(local.d === "1979-05-27" && local.t === "07:32:00",
        "local date and time parse as strings too");
}

remove(ENV_FILE);
print("test_config_upgrade: all tests passed (" + n + " assertions)");
