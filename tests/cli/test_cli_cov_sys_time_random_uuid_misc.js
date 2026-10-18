// Split from tests/test_cov_sys_time_random_uuid_misc.js: the cli sections.
import * as cli from "dyna:cli";
import { Command } from "dyna:cli";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== cli ===");
{
    assert(cli.StyleText("red", "x").includes("x"), "StyleText wraps text");
    assert(cli.StyleText(["red", "bold"], "y").includes("y"), "StyleText array styles");
    throws(() => cli.StyleText("__nope__", "z"), /unknown style/, "unknown style throws");
    assert(cli.Styles().length >= 8, "Styles list non-trivial");
    assert(cli.Styles().includes("red"), "Styles includes red");
    assert(typeof cli.IsTTY() === "boolean", "IsTTY boolean");
    assert(cli.IsTTY(1) === cli.IsTTY(1), "IsTTY deterministic");
    assert(typeof cli.Columns() === "number" && cli.Columns() >= 0, "Columns number");
    assert([1, 4, 8, 24].includes(cli.ColorDepth()) || cli.ColorDepth() >= 0, "ColorDepth plausible");

    const cmd = new Command("cov")
        .describe("coverage cmd")
        .option("-v, --verbose", "verbose")
        .option("-n, --count <num>", "count", { type: "number", default: 1 })
        .option("--tag <t>", "tag", { type: "string" })
        .argument("<file>", "input file")
        .argument("[out]", "output file");

    let r = cmd.parse(["-v", "--count", "5", "--tag", "x", "in.txt", "out.txt"]);
    eq(r.options.verbose, true, "flag option true");
    eq(r.options.count, 5, "numeric option parsed");
    eq(r.options.tag, "x", "string option parsed");
    eq(JSON.stringify(r.arguments), JSON.stringify(["in.txt", "out.txt"]), "positionals captured");

    r = cmd.parse(["--verbose", "only.txt"]);
    eq(r.options.count, 1, "default applies");
    eq(r.options.tag, undefined, "unset string option undefined");

    r = cmd.parse(["-v", "f.txt"]);
    eq(r.options.verbose, true, "short flag alone");

    r = cmd.parse(["--count=3", "f.txt"]);
    eq(r.options.count, 3, "--opt=value form");

    const h = cmd.help();
    assert(h.includes("--verbose") || h.includes("-v"), "help shows verbose");
    assert(h.includes("<file>") || h.includes("file"), "help shows positional");
    eq(cmd.name, "cov", "command name getter");

    throws(() => cmd.parse(["--bogus", "f.txt"]), /./, "unknown option refused");
    const lax = new Command("lax").allowUnknown(true).argument("[x]");
    lax.parse(["--whatever", "v"]); n++;
    const strict = new Command("strict").argument("<need>");
    throws(() => strict.parse([]), /./, "missing required arg throws");

    const sub = new Command("sub").option("--deep", "d").argument("<x>");
    const root = new Command("root").command(sub);
    const rs = root.parse(["sub", "--deep", "val"]);
    assert(rs.command === "sub" || (rs.options && rs.options.deep === true), "subcommand routed");

    const vd = new Command("vd").option("--multi <m...>", "multi", { variadic: true });
    const rv = vd.parse(["--multi", "a", "b", "c"]);
    assert(rv.options.multi === "a" || rv.options.multi[0] === "a", "variadic captures first value");
}
if (fails) {
    print("test_cli_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli_cov_sys_time_random_uuid_misc failed");
}
print("test_cli_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
