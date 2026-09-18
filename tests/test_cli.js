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
 * and styleText would be the synonym the conventions forbid. */
eq(StyleText("red", "x"), "\u001B[31mx\u001B[39m", "a single style");
eq(StyleText("bold", "x"), "\u001B[1mx\u001B[22m", "bold has its own reset");
eq(StyleText(["red", "bold"], "x"),
   "\u001B[31m\u001B[1mx\u001B[22m\u001B[39m",
   "an array composes, and closes in reverse order");
eq(StyleText("red", ""), "\u001B[31m\u001B[39m", "empty text still brackets");
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

eq(StyleText("reset", "x"), "\u001B[0mx\u001B[0m", "reset closes with a second 0m like Node");
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

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
