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
assert(Number.isInteger(Columns()) && Columns() > 0, "Columns is a positive integer");
{
    const d = ColorDepth(1);
    assert([0, 4, 8, 24].indexOf(d) >= 0, "ColorDepth is 0, 4, 8 or 24 (got " + d + ")");
    /* Not a TTY here, so it must say no colour -- and that is the whole point
     * of asking before styling. */
    if (!IsTTY(1)) eq(d, 0, "a non-TTY reports no colour");
}

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
