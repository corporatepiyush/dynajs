// covers: dyna:config.Env.parse
// F-cfg-1: Env.stringify writes U+0000 inside a double-quoted value as the escape \0, but Env.parse only expanded
// \n \t \r and dropped the backslash before anything else, so parse(stringify({A:"x \0"})).A was "x 0". parse now
// expands \0 to U+0000 as Env.load with expand already did (dynajs.d.ts lists the escapes). Oracle: the round trip
// parse(stringify(r)) === r for values holding every escape stringify emits; Env.load still refuses a NUL value
// (setenv cannot carry it), now as the documented TypeError instead of silently loading "x 0".
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env_parse_nul_escape");
import { Env } from "dyna:config";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

__kitS.section("parse(stringify(r)) gives back values with NUL (the ISSUES repro first)", () => {
    eq(Env.parse(Env.stringify({ A: "x \0" })).A, "x \0", "ISSUES repro");
    const rec = { A: "\0", B: "a\0b", C: "\0\0 \0", D: "tab\tnl\ncr\r\0q\"bs\\", E: "\u00e9\0\u65e5 x" };
    const back = Env.parse(Env.stringify(rec));
    for (const k of Object.keys(rec)) eq(back[k], rec[k], "key " + k);
});

__kitS.section("the \\0 escape in a double-quoted value is U+0000; single quotes stay literal", () => {
    eq(Env.parse("A=\"x\\0y\"\n").A, "x\0y", "double-quoted \\0");
    eq(Env.parse("A='x\\0y'\n").A, "x\\0y", "single-quoted text is literal");
    eq(Env.parse("A=\"\\q\"\n").A, "q", "an unknown escape still drops the backslash");
});

__kitS.section("Env.load refuses a NUL value instead of loading the old \"x 0\"", () => {
    const dir = makeTempDir("dyna-env-nul-");
    const p = dir.join("a.env");
    writeFile(p, Env.stringify({ DYNA_TEST_ENV_NUL: "x \0" }));
    let msg = "no throw";
    try { Env.load(p.toString(), { assign: false }); } catch (e) { msg = e.constructor.name + ": " + e.message; }
    ok(/^TypeError: .*NUL/.test(msg), "load rejects the NUL value (" + msg + ")");
    removeAll(dir);
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_config_env_parse_nul_escape: " + n + " checks passed");
__kitS.end();
