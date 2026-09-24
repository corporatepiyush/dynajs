// flags: --std
/* review probe: Command {env}/{version}/action precedence and scoping. */
import { Command } from "dyna:cli";
import * as std from "std";
let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    ok(a === b, msg + "\n  got:  " + JSON.stringify(a) + "\n  want: " + JSON.stringify(b));
}
function err(fn, cls, msg) {
    let e = null; try { fn(); } catch (x) { e = x; }
    ok(e && e.constructor.name === cls,
       msg + " (want " + cls + ", got " + (e ? e.constructor.name + ": " + e.message : "no throw") + ")");
    return e ? e.message : "";
}

/* C1 three-level precedence: default < env < CLI */
std.setenv("E3_LEVEL", "2");
const c1 = new Command("t")
    .option("--level <n>", "lvl", { type: "number", env: "E3_LEVEL", default: 1 });
eq(c1.parse([]).options.level, 2, "C1a env beats default");
eq(c1.parse(["--level", "3"]).options.level, 3, "C1b CLI beats env");
std.unsetenv("E3_LEVEL");
eq(c1.parse([]).options.level, 1, "C1c no env -> default");

/* C2 invalid number env names the variable */
std.setenv("E3_LEVEL", "abc");
let m = err(() => c1.parse([]), "TypeError", "C2 invalid number env refused");
ok(m.indexOf("E3_LEVEL") >= 0, "C2 the message names the variable: " + m);
eq(c1.parse(["--level", "7"]).options.level, 7,
   "C2b an invalid env does NOT refuse when the CLI supplies the value (env is lazy: the CLI is resolved first)");
{
    /* a poisoned variable also cannot poison the OTHER reads of the same
       command: only the options the CLI leaves silent are consulted */
    std.setenv("E3_OTHER", "junk");
    const co = new Command("t").option("--level <n>", "l", { type: "number", env: "E3_LEVEL", default: 1 })
                               .option("--other <x>", "o", { env: "E3_OTHER" });
    eq(co.parse(["--level", "7"]).options.level, 7, "C2c the CLI-resolved option ignores the poisoned variable");
    let e2 = null;
    try { co.parse(["--level", "7"]); } catch (x) { e2 = x; }
    ok(e2 === null, "C2c repeated: no throw at all");
    let e3 = null;
    try { co.parse([]); } catch (x) { e3 = x; }
    ok(e3 && e3.constructor.name === "TypeError", "C2c while an option the CLI omitted still consults its variable (and refuses the poisoned one)");
    std.unsetenv("E3_OTHER");
}
std.unsetenv("E3_LEVEL");

/* C3 boolean env truth table */
const cb = new Command("t").option("--flag", "f", { type: "boolean", env: "E3_FLAG" });
const falsy = ["0", "false", "FALSE", "False", "no", "NO", "n", "N", "off", "OFF", ""];
for (const v of falsy) {
    std.setenv("E3_FLAG", v);
    eq(cb.parse([]).options.flag, false, "C3 bool env " + JSON.stringify(v) + " -> false");
}
const truthy = ["1", "true", "yes", "y", "x", " ", "00", "2"];
for (const v of truthy) {
    std.setenv("E3_FLAG", v);
    eq(cb.parse([]).options.flag, true, "C3 bool env " + JSON.stringify(v) + " -> true");
}
std.unsetenv("E3_FLAG");
eq(cb.parse([]).options.flag, undefined, "C3 unset bool env leaves the option absent");

/* C3b variadic + env: the env value seeds the array ONLY when the CLI is
   silent; a CLI occurrence is resolved first and the variable is never
   consulted (nor validated) in that case */
std.setenv("E3_V", "e1");
const cv = new Command("t").option("--v <x>", "v", { variadic: true, env: "E3_V", default: ["d1"] });
const pv = cv.parse(["--v", "c1", "--v", "c2"]).options.v;
eq(JSON.stringify(pv), '["d1","c1","c2"]', "C3b CLI first: values append to the default, the env seed is not consulted");
eq(JSON.stringify(cv.parse([]).options.v), '["e1"]', "C3b2 with the CLI silent the env value seeds a one-element array");
std.unsetenv("E3_V");

/* C4 version beats required */
const c4 = new Command("t").version("9.9").option("--need <x>", "n", { required: true });
const pv4 = c4.parse(["--version"]);
eq(pv4.version, "9.9", "C4a --version answers although --need is missing");
ok(pv4.result === undefined, "C4b no action ran");
let threw = false;
try { c4.parse([]); } catch (e) { threw = true; }
ok(threw, "C4c without --version the required refusal still throws");

/* C5 --version=value refused */
const m5 = err(() => c4.parse(["--version=x"]), "TypeError", "C5 --version=x refused");
ok(m5.indexOf("takes no value") >= 0, "C5b message says flag takes no value: " + m5);

/* C6 user-registered --version refused in BOTH orders */
err(() => new Command("t").version("1").option("--version", "v"), "TypeError",
    "C6a version() then option(--version) refused");
err(() => new Command("t").option("--version", "v").version("1"), "TypeError",
    "C6b option(--version) then version() refused");
/* C6c --version as a plain user flag is fine when version() is never called */
const c6c = new Command("t").option("--version", "v");
eq(c6c.parse(["--version"]).options.version, true, "C6c no version() -> --version is a normal flag");

/* C7 {env} on a subcommand option */
std.setenv("E3_SUB", "sv");
const sub = new Command("sub").option("--opt <x>", "o", { env: "E3_SUB" });
const root = new Command("root").command(sub);
eq(root.parse(["sub"]).result.options.opt, "sv", "C7 subcommand env default applies");
eq(root.parse(["sub", "--opt", "cli"]).result.options.opt, "cli", "C7b CLI beats env on sub");
std.unsetenv("E3_SUB");

/* C8 two options sharing one env var both get it */
std.setenv("E3_SHARED", "both");
const c8 = new Command("t").option("--a <x>", "a", { env: "E3_SHARED" })
                           .option("--b <x>", "b", { env: "E3_SHARED" });
const p8 = c8.parse([]).options;
eq(p8.a, "both", "C8 first option sees the shared env");
eq(p8.b, "both", "C8b second option sees the shared env");
std.unsetenv("E3_SHARED");

/* C9 empty-string env */
std.setenv("E3_S", "");
const c9s = new Command("t").option("--s <x>", "s", { env: "E3_S", default: "D" });
eq(c9s.parse([]).options.s, "", "C9a empty env string beats the default");
std.setenv("E3_N", "");
const c9n = new Command("t").option("--n <x>", "n", { type: "number", env: "E3_N" });
const m9 = err(() => c9n.parse([]), "TypeError", "C9b empty number env refused like a CLI empty value");
ok(m9.indexOf("E3_N") >= 0, "C9b names the variable: " + m9);
std.setenv("E3_B", "");
const c9b = new Command("t").option("--b", "b", { type: "boolean", env: "E3_B" });
eq(c9b.parse([]).options.b, false, "C9c empty bool env -> false");
std.unsetenv("E3_S"); std.unsetenv("E3_N"); std.unsetenv("E3_B");

/* C10 version scope follows dispatch */
const vsub = new Command("sub").version("2.0").option("--sx <x>", "x");
const vroot = new Command("tool").version("1.0").command(vsub)
    .option("--rx <x>", "x");
eq(vroot.parse(["--version", "sub"]).version, "1.0", "C10a tool --version sub -> root version");
eq(vroot.parse(["sub", "--version"]).result.version, "2.0", "C10b tool sub --version -> sub version");
/* C10c a sub without version() refuses --version as unknown */
const bare = new Command("bare");
const vroot2 = new Command("tool").version("1.0").command(bare);
err(() => vroot2.parse(["bare", "--version"]), "TypeError",
    "C10c a sub without version() refuses --version");

/* C11 `--` terminator hides --version */
const r11 = vroot.parse(["--", "--version"]);
eq(r11.version, undefined, "C11 --version after -- is not the version flag");
eq(r11.arguments[0], "--version", "C11b it is a positional");

/* C12 a value that SPELLS --version is never the flag */
const r12 = vroot.parse(["--rx", "--version"]);
eq(r12.options.rx, "--version", "C12 --version consumed as an option value");

/* C13 an unknown option earlier does NOT beat --version (pin whichever way) */
let s13;
try { s13 = JSON.stringify(Object.keys(vroot.parse(["--bad", "--version"]))); }
catch (e) { s13 = "THREW:" + e.message; }
print("PIN C13 unknown-option + --version -> " + s13);

/* C13b required missing + late --version */
const c13 = new Command("t").version("5").option("--need <x>", "n", { required: true });
eq(c13.parse(["--need2oops", "--version"]).version !== undefined, true,
   "C13b pin: --version still wins over an earlier unknown token? (value above)");

/* C14 parse() with no args uses scriptArgs */
const savedArgs = scriptArgs;
print("PIN C14 scriptArgs writable: " + (() => { try { scriptArgs = ["a"]; const r = new Command("t").parse(); return JSON.stringify(r.arguments); } catch (e) { return "RO:" + e.constructor.name; } })());
try { scriptArgs = savedArgs; } catch (e) {}

/* C15 action basics */
let ran = 0;
const c15 = new Command("t").option("--x", "x").action((o, a) => { ran++; return "R:" + a.join(","); });
const r15 = c15.parse(["a", "b"]);
eq(r15.result, "R:a,b", "C15a action return rides in result");
eq(ran, 1, "C15b action ran exactly once");
err(() => new Command("t").action(42), "TypeError", "C15d action(42) refused");
err(() => new Command("t").action("fn"), "TypeError", "C15e action(string) refused");
err(() => new Command("t").action(), "TypeError", "C15f action() refused");
/* action not run on --version */
let ran2 = 0;
const c15g = new Command("t").version("1").action(() => { ran2++; });
c15g.parse(["--version"]);
eq(ran2, 0, "C15g action does not run on --version");
/* action exceptions propagate */
const c15h = new Command("t").action(() => { throw new RangeError("boom"); });
err(() => c15h.parse([]), "RangeError", "C15h action exceptions propagate");

/* C16 version() getter/setter/validation */
const c16 = new Command("t");
eq(c16.version(), "", "C16a unset version reads back empty");
eq(c16.version("1.2"), c16, "C16b setter is chainable");
eq(c16.version(), "1.2", "C16c getter returns what was set");
err(() => c16.version(5), "TypeError", "C16d version(5) refused");
err(() => c16.version(null), "TypeError", "C16e version(null) refused");

/* C17 help() shows Version and (env: NAME) */
std.setenv("E3_H", "x");
const c17 = new Command("t").version("3.4").option("--o <x>", "o", { env: "E3_H" });
const h = c17.help();
ok(h.indexOf("Version: 3.4") >= 0, "C17a help shows the version line");
ok(h.indexOf("(env: E3_H)") >= 0, "C17b help notes the env var");
std.unsetenv("E3_H");

/* C18 env var with invalid name refused at registration */
err(() => new Command("t").option("--o <x>", "o", { env: "A=B" }), "TypeError", "C18a env with = refused");
err(() => new Command("t").option("--o <x>", "o", { env: "" }), "TypeError", "C18b empty env name refused");
err(() => new Command("t").option("--o <x>", "o", { env: 5 }), "TypeError", "C18c non-string env refused");

/* C19 number env special values: one strict decimal grammar shared by CLI and
   env. The JS spellings are refused, never silently coerced ("NaN" would be a
   NaN, "Infinity" a valid double, "0x10" the number 16). */
function numEnv(varname, value, label) {
    std.setenv(varname, value);
    const c = new Command("t").option("--n <x>", "n", { type: "number", env: varname });
    let v;
    try { v = String(c.parse([]).options.n); } catch (e) { v = "THREW:" + e.constructor.name; }
    std.unsetenv(varname);
    return label + " env=" + JSON.stringify(value) + " -> " + v;
}
{
    const rows = [
        [numEnv("E3_NAN", "NaN", "C19"), 'C19 env="NaN" -> THREW:TypeError'],
        [numEnv("E3_INF", "Infinity", "C19b"), 'C19b env="Infinity" -> THREW:TypeError'],
        [numEnv("E3_HEX", "0x10", "C19c"), 'C19c env="0x10" -> THREW:TypeError'],
        [numEnv("E3_NAN", "nAn", "C19d"), 'C19d env="nAn" -> THREW:TypeError'],
        [numEnv("E3_OK", "1e3", "C19e"), 'C19e env="1e3" -> 1000'],
        [numEnv("E3_OK2", " -.5 ", "C19f"), 'C19f env=" -.5 " -> -0.5'],
    ];
    for (const [got, want] of rows) eq(got, want, "number env grammar");
}
{
    /* and the CLI path refuses the identical spellings: the grammars cannot
       drift apart */
    const cn = new Command("t").option("--n <x>", "n", { type: "number" });
    for (const bad of ["NaN", "Infinity", "0x10"]) {
        let v;
        try { v = String(cn.parse(["--n", bad]).options.n); } catch (e) { v = "THREW:" + e.constructor.name; }
        eq(v, "THREW:TypeError", "C19g the CLI refuses " + JSON.stringify(bad) + " exactly like the env path");
    }
    eq(cn.parse(["--n", "1e3"]).options.n, 1000, "C19h and accepts 1e3 exactly like the env path");
}

print("probe_cmd: " + (n - fails) + "/" + n + " ok");
if (fails) throw new Error("probe_cmd failures: " + fails);
