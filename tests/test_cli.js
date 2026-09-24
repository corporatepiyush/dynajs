// flags: --std
/* test_cli.js -- Command, StyleText and the TTY queries in dyna:cli (design 14).
 *
 * An argument parser is only as good as the argv shapes it refuses, so most of
 * this file is the forms that separate a real parser from a split-on-space:
 * `--` termination, `-abc` bundling, `-n5`, `--no-x` negation, and unknown
 * options failing loudly rather than being swallowed as positionals.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_cli.js
 */
import { Command, StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const base = () => new Command("tool")
    .option("-v, --verbose", "chatty", { type: "boolean" })
    .option("-o, --out <path>", "output file", { type: "string" })
    .option("-n, --count <n>", "how many", { type: "number", default: 1 });

/* ------------------------------------------------------------- long form */

{
    const r = base().parse(["--verbose", "--out", "f.txt", "--count", "7"]);
    eq(r.options.verbose, true, "--verbose");
    eq(r.options.out, "f.txt", "--out with a separate value");
    eq(r.options.count, 7, "--count coerces to a number");
    eq(typeof r.options.count, "number", "the declared type is honoured");
    eq(r.arguments.length, 0, "no positionals");
    eq(r.command, null, "no subcommand");
}
eq(base().parse(["--out=f.txt"]).options.out, "f.txt", "--out=value");
eq(base().parse(["--count=42"]).options.count, 42, "--count=value coerces");
eq(base().parse([]).options.count, 1, "a default applies when absent");
eq(base().parse([]).options.verbose, undefined, "no default means undefined");

/* Negation, which is what makes a boolean default-true usable. */
{
    const c = new Command("t").option("--color", "colour", { type: "boolean", default: true });
    eq(c.parse([]).options.color, true, "default true");
    eq(c.parse(["--no-color"]).options.color, false, "--no-color negates");
    eq(c.parse(["--color"]).options.color, true, "--color sets");
    /* A value on a flag used to be silently discarded while setting true. */
    throws(() => base().parse(["--verbose=notabool"]),
           "a value on a boolean flag is refused, not discarded");
    throws(() => base().parse(["--no-color=x"]),
           "and the negated form refuses one too");
    eq(base().parse(["--verbose"]).options.verbose, true,
       "the bare flag still sets");
}

/* ------------------------------------------------------------ short form */

eq(base().parse(["-v"]).options.verbose, true, "-v");
eq(base().parse(["-o", "f"]).options.out, "f", "-o with a separate value");
eq(base().parse(["-n5"]).options.count, 5, "-n5 attaches its value");
eq(base().parse(["-ofile"]).options.out, "file", "-ofile attaches its value");
/* Bundling: -vv is two booleans, and a value option ends the bundle. */
{
    const c = new Command("t")
        .option("-a, --alpha", "a", { type: "boolean" })
        .option("-b, --beta", "b", { type: "boolean" })
        .option("-c, --charlie <v>", "c", { type: "string" });
    const r = c.parse(["-ab"]);
    eq(r.options.alpha, true, "-ab sets alpha");
    eq(r.options.beta, true, "-ab sets beta");
    eq(c.parse(["-abcX"]).options.charlie, "X", "a value option ends the bundle");
}

/* ------------------------------------------------------ positionals and -- */

{
    const r = base().parse(["a", "b", "c"]);
    eq(r.arguments.length, 3, "positionals collect");
    eq(r.arguments[0], "a", "positional order");
}
{
    /* THE `--` TERMINATOR: everything after it is a positional even if it
     * looks exactly like an option. Without this a CLI cannot pass flags on. */
    const r = base().parse(["-v", "--", "--out", "-n5"]);
    eq(r.options.verbose, true, "options before -- still parse");
    eq(r.arguments.length, 2, "everything after -- is positional");
    eq(r.arguments[0], "--out", "a flag after -- is NOT parsed as one");
    eq(r.arguments[1], "-n5", "including an attached-value form");
    eq(r.options.out, undefined, "--out after -- did not set the option");
}
eq(base().parse(["-"]).arguments[0], "-", "a bare - is a positional, not a flag");
/* A token that parses ENTIRELY as a number is a value, not an unknown
   option: CLIs take negative offsets, and `-v` is still an option. */
eq(base().parse(["-5"]).arguments[0], "-5", "a negative number is a positional");
eq(base().parse(["-.5"]).arguments[0], "-.5", "a negative decimal too");
eq(base().parse(["-v"]).options.verbose, true, "while -v is still an option");
{
    /* Dispatch must not look for a subcommand PAST the terminator: after `--`
       everything is positional, so nothing there can be dispatched. */
    const root = new Command("tool").command(new Command("add"));
    const r = root.parse(["--", "add"]);
    eq(r.command, null, "a subcommand cannot hide behind --");
    eq(r.arguments[0], "add", "the token after -- stays a positional");
    eq(root.parse(["add"]).command, "add", "without -- it still dispatches");
}

/* -------------------------------------------------------------- variadic */

{
    const c = new Command("t").option("-I, --include <p>", "paths",
                                      { type: "string", variadic: true });
    const r = c.parse(["-I", "a", "-I", "b", "--include", "c"]);
    assert(Array.isArray(r.options.include), "a variadic option collects an array");
    eq(r.options.include.length, 3, "all three occurrences");
    eq(r.options.include[2], "c", "long and short forms collect together");
}
/* A NON-variadic option repeated takes the last value, not an array. */
eq(base().parse(["-o", "a", "-o", "b"]).options.out, "b",
   "a repeated non-variadic option takes the last value");

/* ------------------------------------------------------------- refusals */

throws(() => base().parse(["--nope"]), "an unknown long option is refused");
throws(() => base().parse(["-z"]), "an unknown short option is refused");
throws(() => base().parse(["--out"]), "a value option with no value is refused");
throws(() => base().parse(["-o"]), "the short form too");
throws(() => base().parse(["--count", "abc"]), "a non-numeric value for a number option");
throws(() => base().parse("not an array"), "parse refuses a non-array");
throws(() => new Command("t").option("noflags"), "an option with no --long is refused");
throws(() => new Command("t").option("--"), "an option with an EMPTY long name is refused");
throws(() => new Command("t").option("-v, --verbose", "a").option("--verbose", "b"),
       "a duplicate long name is refused at the declaration");
{
    const root = new Command("t").command(new Command("add"));
    throws(() => root.command(new Command("add")),
           "a duplicate subcommand name is refused at the declaration");
}
throws(() => new Command("t").option("-v", "x", { type: "purple" }), "an unknown type is refused");
{
    const c = new Command("t").option("-r, --req <v>", "r", { type: "string", required: true });
    throws(() => c.parse([]), "a required option that is absent is refused");
    eq(c.parse(["-r", "x"]).options.req, "x", "and satisfied when present");
}
{
    const c = new Command("t").argument("<input>", "the input");
    throws(() => c.parse([]), "a required argument that is absent is refused");
    eq(c.parse(["f"]).arguments[0], "f", "and satisfied when present");
    eq(new Command("t").argument("[opt]", "o").parse([]).arguments.length, 0,
       "an optional argument may be absent");
}
/* allowUnknown collects instead of throwing -- opt-in, because silently
 * swallowing a typo'd flag is how a script does the wrong thing quietly. */
{
    const c = base().allowUnknown();
    const r = c.parse(["--nope", "-v"]);
    eq(r.options.verbose, true, "known options still parse under allowUnknown");
    assert(r.arguments.indexOf("--nope") >= 0, "the unknown flag is kept as a positional");
}

/* ----------------------------------------------------------- subcommands */

{
    const add = new Command("add").describe("add a thing")
        .option("-f, --force", "force", { type: "boolean" });
    const root = new Command("tool").option("-v, --verbose", "v", { type: "boolean" })
        .command(add);
    const r = root.parse(["-v", "add", "-f", "x"]);
    eq(r.command, "add", "the subcommand is named");
    eq(r.options.verbose, true, "the root's options still parse");
    eq(r.result.options.force, true, "the subcommand's options parse");
    eq(r.result.arguments[0], "x", "the subcommand's positionals");
    /* A token that is not a subcommand stays a positional. */
    eq(root.parse(["other"]).command, null, "an unknown token is not a subcommand");
    eq(root.parse(["other"]).arguments[0], "other", "and stays a positional");
}
throws(() => new Command("t").command("notacommand"), "command() requires a Command");

/* ------------------------------------------------------------------ help */

{
    const c = new Command("tool").describe("does a thing")
        .option("-v, --verbose", "chatty", { type: "boolean" })
        .option("-o, --out <path>", "output file", { type: "string" })
        .argument("<input...>", "inputs")
        .command(new Command("sub").describe("a subcommand"));
    const h = c.help();
    assert(h.indexOf("Usage: tool") === 0, "help starts with a usage line");
    assert(h.indexOf("[options]") > 0, "usage mentions options");
    assert(h.indexOf("<input...>") > 0, "usage shows the variadic argument");
    assert(h.indexOf("does a thing") > 0, "help carries the description");
    assert(h.indexOf("-v, --verbose") > 0, "help lists the short and long form");
    assert(h.indexOf("--out <path>") > 0, "help shows the placeholder");
    assert(h.indexOf("chatty") > 0, "help carries option descriptions");
    assert(h.indexOf("Commands:") > 0, "help lists subcommands");
    assert(h.indexOf("sub") > 0, "the subcommand is named");
    /* Help is width-aware through design 10's owner of width math. */
    for (const line of h.split("\n"))
        assert(line.displayWidth() === line.stripAnsi().displayWidth(),
               "help lines carry no stray escapes");
}
eq(new Command("tool").name, "tool", "the name getter");

/* ------------------------------------------------------------- StyleText */

/* Node's util.styleText signature, deliberately -- a third spelling after chalk
 * and styleText would be the synonym the conventions forbid. FORCE_COLOR=1
 * pins emission: the auto-dim default (dimmed on a piped stdout) is asserted
 * in test_term.js; here the bytes are the contract. */
{
    const had = std.getenv("FORCE_COLOR");
    std.setenv("FORCE_COLOR", "1");
    eq(StyleText("red", "x"), "\u001B[31mx\u001B[39m", "a single style");
    eq(StyleText("bold", "x"), "\u001B[1mx\u001B[22m", "bold has its own reset");
    eq(StyleText(["red", "bold"], "x"),
       "\u001B[31m\u001B[1mx\u001B[22m\u001B[39m",
       "an array composes, and closes in reverse order");
    eq(StyleText("red", ""), "\u001B[31m\u001B[39m", "empty text still brackets");
    if (had === undefined) std.unsetenv("FORCE_COLOR"); else std.setenv("FORCE_COLOR", had);
}
/* The styled text must survive the width math it will be measured with. */
eq(StyleText(["green", "underline"], "hi").stripAnsi(), "hi", "stripAnsi recovers the text");
eq(StyleText(["green", "underline"], "hi").displayWidth(), 2, "styling adds no width");
throws(() => StyleText("chartreuse", "x"), "an unknown style is REFUSED, not ignored");
throws(() => StyleText("red"), "StyleText needs both arguments");
{
    const list = Styles();
    assert(Array.isArray(list) && list.length > 10, "Styles() enumerates the set");
    for (const s of list)
        assert(typeof StyleText(s, "x") === "string",
               "every style Styles() lists actually works: " + s);
}

/* ------------------------------------------------------------------- TTY */

assert(typeof IsTTY(1) === "boolean", "IsTTY returns a boolean");
assert(typeof IsTTY() === "boolean", "IsTTY with no fd reads no out-of-range argv");
assert(Number.isInteger(Columns()) && Columns() > 0, "Columns is a positive integer");
{
    const d = ColorDepth(1);
    assert([0, 4, 8, 24].indexOf(d) >= 0, "ColorDepth is 0, 4, 8 or 24 (got " + d + ")");
    /* Not a TTY here, so it must say no colour -- and that is the whole point
     * of asking before styling. */
    if (!IsTTY(1)) eq(d, 0, "a non-TTY reports no colour");
}
/* FORCE_COLOR forces a depth whatever the TTY says; NO_COLOR is a refusal
 * only where nothing forces. The env is restored so later runs are clean. */
{
    const { setenv, unsetenv } = std;
    const depth = () => ColorDepth(1);
    unsetenv("NO_COLOR");
    setenv("FORCE_COLOR", "1");
    eq(depth(), 4, "FORCE_COLOR=1 forces 16-colour on a non-TTY");
    setenv("FORCE_COLOR", "true");
    eq(depth(), 4, "FORCE_COLOR=true too");
    setenv("FORCE_COLOR", "2");
    eq(depth(), 8, "FORCE_COLOR=2 is 256");
    setenv("FORCE_COLOR", "3");
    eq(depth(), 24, "FORCE_COLOR=3 is truecolor");
    setenv("NO_COLOR", "1");
    eq(depth(), 24, "and FORCE_COLOR overrides NO_COLOR");
    setenv("FORCE_COLOR", "0");
    eq(depth(), 0, "FORCE_COLOR=0 refuses");
    setenv("FORCE_COLOR", "false");
    eq(depth(), 0, "and FORCE_COLOR=false too");
    setenv("FORCE_COLOR", "9");
    eq(depth(), 4, "any other non-empty value is level 1");
    unsetenv("FORCE_COLOR");
    unsetenv("NO_COLOR");
    setenv("NO_COLOR", "1");
    if (IsTTY(1))
        eq(depth(), 0, "NO_COLOR disables colour on a real TTY");
    else
        eq(depth(), 0, "NO_COLOR set and non-empty disables (non-TTY is 0 anyway)");
    unsetenv("NO_COLOR");
}

/* ------------------------------------------------- defaults are not aliased */

/* A spec default is written once; a variadic push into, or a plain write to,
 * the parse result used to mutate the registration and leak into every later
 * parse. */
{
    const c = new Command("t").option("--tag <v>", "t",
                                      { type: "string", variadic: true, default: ["a"] });
    const r1 = c.parse(["--tag", "b"]);
    eq(r1.options.tag.length, 2, "a variadic option pushes onto its default");
    eq(JSON.stringify(c.parse([]).options.tag), "[\"a\"]",
       "an earlier parse did not mutate the spec default");
}
{
    const c = new Command("t").option("--o <v>", "o", { type: "string", default: { k: 1 } });
    const r1 = c.parse([]);
    r1.options.o.k = 99;
    eq(c.parse([]).options.o.k, 1, "an object default is copied, not shared");
}
{
    const inner = { v: 2 };
    const c = new Command("t").option("--o <v>", "o",
                                      { type: "string", default: { inner: inner } });
    const r = c.parse([]);
    eq(r.options.o.inner.v, 2, "a nested object default copies structure");
    eq(inner.v, 2, "the copy did not touch the original");
    r.options.o.inner.v = 77;
    eq(inner.v, 2, "writing through the copy does not reach the original");
}

/* ------------------------------------- root requirements hold across subs */

{
    const root = new Command("tool")
        .option("--req <v>", "r", { type: "string", required: true })
        .command(new Command("sub").describe("s"));
    throws(() => root.parse(["sub"]),
           "a root required option is enforced when a subcommand dispatches");
    throws(() => root.parse(["sub", "--req", "v"]),
           "a root option after the subcommand cannot satisfy the requirement");
    eq(root.parse(["--req", "v", "sub"]).options.req, "v",
       "and the requirement is satisfied by the prefix");
}
throws(() => new Command("tool").argument("<needed>")
           .command(new Command("sub")).parse(["sub"]),
       "a root required argument is enforced when a subcommand dispatches");

/* --------------------------------------------------- parse() defaults argv */

{
    const r = new Command("t").parse();
    eq(r.command, null, "parse() with no argument parses scriptArgs");
    eq(r.arguments.length, scriptArgs.length, "the engine's argv, token for token");
}

/* The no-arg path must RELEASE its scriptArgs reference even on the argv-cap
   refusal: the row below leaks one reference and this process's teardown is
   the detector -- QuickJS asserts its GC list is empty at JS_FreeRuntime and
   aborts with rc 134 if the reference escaped (a leak sanitizer is blind to
   this class). */
{
    const saved = scriptArgs;
    let cls = "readonly";
    try {
        scriptArgs = new Array(70000).fill("y");
        cls = "";
        try { new Command("t").parse(); } catch (e) { cls = e.constructor.name; }
    } catch (e) { /* scriptArgs not writable in this build: nothing to test */ }
    try { scriptArgs = saved; } catch (e) {}
    if (cls !== "readonly")
        eq(cls, "RangeError",
           "a 70000-token scriptArgs is refused with RangeError AND its reference released");
}

/* ------------------------------- number values: one strict decimal grammar */

{
    const c = new Command("t").option("--n <v>", "n", { type: "number" });
    eq(c.parse(["--n", "8"]).options.n, 8, "an integer parses");
    eq(c.parse(["--n", "-.5"]).options.n, -0.5, "a sign and a bare fraction parse");
    eq(c.parse(["--n", "1e3"]).options.n, 1000, "an exponent parses");
    eq(c.parse(["--n", " 5 "]).options.n, 5, "surrounding ASCII whitespace is ignored");
    /* The value is a spelling, not a JS numeric literal: JS would take
       "0x10" as 16, "Infinity" as a valid double and "NaN" as a valid NaN,
       each silently. */
    for (const bad of ["NaN", "nAn", "Infinity", "-Infinity", "0x10", "0b1",
                       "1_000", "abc", "", ".", "+", "1e", "1e999", "5x"]) {
        let cls = "";
        try { c.parse(["--n", bad]); } catch (e) { cls = e.constructor.name; }
        eq(cls, "TypeError", "the CLI refuses " + JSON.stringify(bad) + " as a number");
    }
    /* and the env path shares the grammar exactly: same accept, same refuse */
    std.setenv("E3_NUM", "0x10");
    const ce = new Command("t").option("--n <v>", "n", { type: "number", env: "E3_NUM" });
    let cls = "";
    try { ce.parse([]); } catch (e) { cls = e.constructor.name; }
    eq(cls, "TypeError", "an env number is refused with the same grammar (0x10)");
    std.setenv("E3_NUM", "1e3");
    eq(ce.parse([]).options.n, 1000, "and accepted with the same grammar (1e3)");
    std.unsetenv("E3_NUM");
}

/* --------------------------- dispatch skips option values on the way to sub */

{
    const sub = new Command("add").describe("add a thing")
        .option("-f, --force", "force", { type: "boolean" });
    const root = new Command("tool")
        .option("-C, --chdir <dir>", "directory", { type: "string" })
        .command(sub);
    const r = root.parse(["-C", "/x", "add", "-f", "t1"]);
    eq(r.command, "add", "dispatch finds the subcommand past an option's value");
    eq(r.options.chdir, "/x", "the root option still parses");
    eq(r.result.options.force, true, "the subcommand's options still parse");
    const r2 = root.parse(["--chdir", "add", "x"]);
    eq(r2.command, null, "an option value that NAMES a subcommand is still a value");
    eq(r2.options.chdir, "add", "the value lands where it belongs");
}

/* ------------------------------------------------- registration refusals */

throws(() => new Command("t").option("-v, --verbose", "a").option("-v, --vtwo", "b"),
       "a duplicate SHORT name is refused at the declaration");
{
    const c = new Command("t");
    throws(() => c.command(c), "a command cannot be its own subcommand");
}

/* -------------------------------------------------------- short =-form */

eq(base().parse(["-o=x"]).options.out, "x", "-o=x attaches x, without the =");
eq(base().parse(["-o="]).options.out, "", "-o= attaches the empty string");

/* --------------------------------------------------------- StyleText, Node */

{
    const had = std.getenv("FORCE_COLOR");
    std.setenv("FORCE_COLOR", "1");
    eq(StyleText("reset", "x"), "\u001B[0mx\u001B[0m", "reset closes with a second 0m like Node");
    if (had === undefined) std.unsetenv("FORCE_COLOR"); else std.setenv("FORCE_COLOR", had);
}
{
    const list = Styles();
    eq(list.length, 46, "the style set matches Node v22's 46 names");
    for (const s of ["grey", "redBright", "bgGray", "blink", "hidden", "framed",
                     "overlined", "doubleunderline", "bgRedBright", "whiteBright"])
        assert(list.indexOf(s) >= 0, "Node's style name is present: " + s);
}
throws(() => StyleText("red", 5), "StyleText refuses a non-string text like Node");
{
    let e = null;
    try { StyleText("chartreuse", "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "TypeError",
           "an unknown style is a TypeError like Node");
}

/* ------------------------------------------------------------- fd refusals */

throws(() => IsTTY("banana"), "IsTTY(fd) refuses a non-number fd");
throws(() => ColorDepth("banana"), "ColorDepth(fd) refuses a non-number fd");

/* ---------------------------------------------- errors carry no raw escapes */

{
    let msg = "";
    try { base().parse(["--\u001B[31mred"]); } catch (e) { msg = e.message; }
    assert(msg.indexOf("\u001B") < 0,
           "an unknown-option error carries no raw ESC: " + JSON.stringify(msg));
    assert(msg.indexOf("\\x1b[31mred") >= 0, "the flag is escaped instead");
    try { base().parse(["--count", "\u001B[31m9"]); } catch (e) { msg = e.message; }
    assert(msg.indexOf("\u001B") < 0, "a coercion error carries no raw ESC either");
}

/* ------------------------------------------------- help: columns and notes */

{
    const h = new Command("t")
        .option("-s, --short", "small", { type: "boolean" })
        .option("-m, --medium-name <p>", "mid", { type: "string" })
        .option("--n <v>", "count", { type: "number", default: 5 })
        .option("--req <v>", "needed", { type: "string", required: true })
        .help();
    const lines = h.split("\n");
    const col = (needle) => {
        for (const l of lines) if (l.indexOf(needle) >= 0) return l.indexOf(needle);
        return -1;
    };
    eq(col("small"), col("mid"), "help descriptions share one column");
    eq(col("mid"), col("count"), "descriptions align across placeholder forms");
    eq(col("count"), col("needed"), "descriptions align across required forms");
    assert(h.indexOf("(required)") > 0, "help marks a required option");
    assert(h.indexOf("(default: 5)") > 0, "help shows a default value");
    for (const line of lines)
        eq(line, line.replace(/\s+$/, ""), "a help line has no trailing padding");
}

/* ------------------------------------------------------- action(fn) */

{
    const c = new Command("t").argument("<x>", "x")
        .action((opts, args) => "ran:" + args[0] + ":" + JSON.stringify(opts));
    const r = c.parse(["v"]);
    eq(r.result, "ran:v:{}", "the action runs on a leaf parse and its return lands on result");
    eq(typeof r.options, "object", "the parse result shape is unchanged");
    throws(() => new Command("t").action(42), "a non-function action is refused");
    throws(() => new Command("t").action(), "an absent action is refused");
    const chain = new Command("t");
    eq(chain.action(() => 1), chain, "action() is chainable");
    /* Only the dispatched leaf runs its handler -- the root's must not fire
       when a subcommand takes the argv. */
    let rootRan = 0, subRan = 0;
    const sub = new Command("sub").action(() => { subRan++; return "sub!"; });
    const root = new Command("root").action(() => { rootRan++; return "root!"; })
        .command(sub);
    const d = root.parse(["sub"]);
    eq(subRan, 1, "the leaf's action runs on dispatch");
    eq(rootRan, 0, "the root's action does not");
    eq(d.result.result, "sub!", "the leaf's return value rides in the dispatch result");
    /* A version answer must NOT run the action. */
    const vc = new Command("t").version("1.0").action(() => "nope");
    eq(vc.parse(["--version"]).version, "1.0", "the version answer carries the string");
    eq(vc.parse(["--version"]).result, undefined, "and the action did not run for it");
}

/* -------------------------------------------------------- version */

{
    const c = new Command("tool").version("2.0.1");
    eq(c.version(), "2.0.1", "version() reads back what was set");
    {
        const chain = new Command("t").version("1");
        eq(chain.version("x"), chain, "version(v) is chainable");
    }
    throws(() => new Command("t").version(5), "a non-string version is refused");
    eq(new Command("t").version(), "", "an unset version reads as the empty string");
    eq(new Command("tool").version("1").parse([]).version, undefined,
       "a plain parse carries no version field");
    const r = c.parse(["--version"]);
    eq(r.version, "2.0.1", "--version answers with the version");
    eq(r.command, null, "and does not dispatch");
    eq(r.arguments.length, 0, "nor parse positionals past it");
    throws(() => c.parse(["--version=3"]), "--version=value is refused: the flag takes none");
    /* Scope: a --version before a subcommand answers for the root; after the
       subcommand token, the subcommand answers with its own. */
    const sub = new Command("sub").version("0.2");
    const root = new Command("root").version("3.0").command(sub);
    eq(root.parse(["--version", "sub"]).version, "3.0", "the root answers before the dispatch point");
    const sr = root.parse(["sub", "--version"]);
    eq(sr.command, "sub", "past the dispatch point the subcommand parses");
    eq(sr.result.version, "0.2", "and answers with its own version");
    /* An option's VALUE that merely spells --version must not trigger it. */
    const oc = new Command("t").version("9").option("--file <f>", "f", { type: "string" });
    eq(oc.parse(["--file", "--version"]).options.file, "--version",
       "a value that spells --version is still a value");
    throws(() => new Command("t").option("--version").version("1"),
           "an explicit --version option collides with the automatic flag: refused");
    throws(() => new Command("t").version("1").option("--version"),
           "the mirror order is refused too");
    eq(new Command("t").option("--version").parse(["--version"]).options.version, true,
       "without .version() a registered --version option parses normally");
    eq(new Command("tool").version("7").help().indexOf("Version: 7") > 0, true,
       "help() shows the version");
    /* -- past the terminator is positional again. */
    eq(c.parse(["--", "--version"]).arguments[0], "--version",
       "--version after -- is a positional");
}

/* -------------------------------------------------- env-var defaults */

{
    const { setenv, unsetenv } = std;
    const had = std.getenv("E3_LEVEL");
    setenv("E3_LEVEL", "7");
    const c = new Command("t").option("--level <n>", "l",
                                      { type: "number", env: "E3_LEVEL", default: 1 });
    eq(c.parse([]).options.level, 7, "the env value seeds an absent option");
    eq(c.parse(["--level", "2"]).options.level, 2, "the CLI beats the env");
    setenv("E3_LEVEL", "not-a-number");
    throws(() => c.parse([]), "a non-numeric env for a number option throws, naming the variable");
    {
        let msg = "";
        try { c.parse([]); } catch (e) { msg = e.message; }
        assert(msg.indexOf("E3_LEVEL") >= 0, "the error names the variable, not just the flag");
    }
    unsetenv("E3_LEVEL");
    eq(c.parse([]).options.level, 1, "without the variable the declared default applies");
    /* boolean env mapping */
    const b = new Command("t").option("--flag", "f", { type: "boolean", env: "E3_FLAG" });
    for (const [val, want] of [["0", false], ["false", false], ["FALSE", false],
                               ["no", false], ["n", false], ["off", false], ["", false],
                               ["1", true], ["true", true], ["anything", true]]) {
        setenv("E3_FLAG", val);
        eq(b.parse([]).options.flag, want, "E3_FLAG=" + JSON.stringify(val) + " maps to " + want);
    }
    unsetenv("E3_FLAG");
    eq(b.parse([]).options.flag, undefined, "an unset env leaves the option undefined");
    /* A poisoned variable must not refuse an invocation that resolves the
       option on the command line: env is consulted (and validated) lazily,
       only where the CLI was silent. */
    setenv("E3_LEVEL", "not-a-number");
    eq(c.parse(["--level", "2"]).options.level, 2,
       "a poisoned env cannot refuse a CLI-supplied option");
    /* variadic: an env value seeds a one-element array when the CLI is
       silent; when the CLI supplies the option the variable is not consulted
       at all, so the CLI values stand alone */
    setenv("E3_TAG", "env1");
    const v = new Command("t").option("--tag <t>", "t",
                                      { type: "string", variadic: true, env: "E3_TAG" });
    eq(JSON.stringify(v.parse([]).options.tag), '["env1"]', "env seeds a variadic array");
    eq(JSON.stringify(v.parse(["--tag", "cli1"]).options.tag), '["cli1"]',
       "a CLI occurrence is resolved first: the env var is not consulted");
    unsetenv("E3_TAG");
    /* env is refused at the declaration, not at parse time */
    throws(() => new Command("t").option("--x <v>", "x", { env: "" }),
           "an empty env name is refused");
    throws(() => new Command("t").option("--x <v>", "x", { env: "A=B" }),
           "an env name carrying '=' is refused");
    throws(() => new Command("t").option("--x <v>", "x", { env: 5 }),
           "a non-string env is refused");
    if (had === undefined) unsetenv("E3_LEVEL"); else setenv("E3_LEVEL", had);
}

/* ------------------------------------------------------ strict option bags */

{
    let msg = "";
    try { new Command("t").option("--x <v>", "x", { typo: 1 }); } catch (e) { msg = e.message; }
    assert(msg.indexOf("typo") >= 0, "an unknown option-bag key names the key: " + msg);
    assert(msg.indexOf("valid:") >= 0, "and names the valid set: " + msg);
    for (const k of ["type", "required", "variadic", "default", "env"])
        assert(msg.indexOf(k) >= 0, "the valid set lists " + k + ": " + msg);
    throws(() => new Command("t").option("--x <v>", "x", "notabag"),
           "a non-object options bag is refused");
    eq(new Command("t").option("--x <v>", "x", undefined).parse(["--x", "1"]).options.x, "1",
       "an explicitly-undefined bag counts as absent");
    eq(new Command("t").option("--x <v>", "x", null).parse(["--x", "1"]).options.x, "1",
       "a null bag counts as absent");
}

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
