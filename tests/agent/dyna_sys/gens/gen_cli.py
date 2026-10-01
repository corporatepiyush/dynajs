#!/usr/bin/env python3
"""gen_cli.py — dyna:cli probes: Command parse matrix, StyleText goldens,
TTY queries."""
from probe_lib import emit

IMP = 'import { Command, StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";\n'

emit("cli", "command", IMP, r'''
const cmd = new Command("deploy")
  .describe("Deploy a service")
  .option("-e, --env <name>", "environment", { required: true })
  .option("-v, --verbose", "verbose logging")
  .option("-r, --retries <n>", "retry count", { type: "number", default: 3 })
  .option("-t, --tag <name>", "repeatable tag", { variadic: true })
  .argument("<service>", "service name");

// the documented matrix
let r = cmd.parse(["-v", "--env", "prod", "--retries", "5", "--tag", "a", "--tag", "b", "api"]);
assert_eq(r.options.env, "prod", "long option value");
assert_eq(r.options.verbose, true, "boolean flag");
assert_eq(r.options.retries, 5, "number option parses");
assert_eq(r.options.tag.join(","), "a,b", "variadic collects repeats");
assert_eq(r.arguments[0], "api", "positional");

// = form
r = cmd.parse(["--env=stage", "web"]);
assert_eq(r.options.env, "stage", "--name=value form");

// short name
r = cmd.parse(["-e", "dev", "db"]);
assert_eq(r.options.env, "dev", "short name works");

// defaults applied
assert_eq(cmd.parse(["--env", "prod", "api"]).options.retries, 3, "default applied");

// unknown option throws; allowUnknown turns them into positionals
assert_throws_msg(() => cmd.parse(["api", "--wat"]), null, "unknown option", "unknown option refused");
const loose = new Command("loose").allowUnknown(true).option("-k, --known", "k");
const lr = loose.parse(["--nope", "pos"]);
assert_true(lr !== null, "allowUnknown parses");

// required missing
assert_throws_msg(() => cmd.parse(["api"]), null, "required", "missing required option throws");
assert_throws_msg(() => cmd.parse(["--env", "prod"]), null, "required argument", "missing required positional throws");

// help text shape
assert_true(cmd.help().startsWith("Usage: deploy"), "help starts with Usage: <name>");
assert_true(cmd.help().indexOf("--env") >= 0, "help lists options");

// number type refuses non-numeric (no implicit coercion)
const num = new Command("n").option("-c, --count <n>", "count", { type: "number" });
assert_throws(() => num.parse(["--count", "abc"]), "TypeError", "non-numeric number option throws TypeError");

// subcommand dispatch
const git = new Command("git").option("-q, --quiet", "quiet")
  .command(new Command("clone").argument("<url>"));
const sub = git.parse(["-q", "clone", "http://x"]);
assert_eq(sub.command, "clone", "subcommand name");
assert_eq(sub.options.quiet, true, "parent options parsed");
assert_eq(sub.result.arguments[0], "http://x", "subcommand positional");

// bounds: the 257th option() registration itself is the RangeError
const many = new Command("many");
assert_throws(() => { for (let i = 0; i < 300; i++) many.option("--opt" + i + " <v>", "o"); },
              "RangeError", "past 256 options RangeError at registration");

// name accessor
assert_eq(new Command("x").name, "x", "name accessor");
assert_eq(new Command().name, "", "default name is empty string");
summary("cli.command");
''')

emit("cli", "style", IMP, r'''
// golden ANSI sequences
assert_eq(JSON.stringify(StyleText("bold", "hi")), '"\\u001b[1mhi\\u001b[22m"', "bold golden");
assert_true(StyleText(["red", "underline"], "x").indexOf("\u001b[31m") >= 0, "array style includes red");
assert_true(StyleText(["red", "underline"], "x").indexOf("\u001b[4m") >= 0, "array style includes underline");
// closed in reverse: the reset for underline (24) appears before red's (39)
{
  const s = StyleText(["red", "underline"], "x");
  assert_true(s.indexOf("\u001b[24m") < s.indexOf("\u001b[39m"), "styles closed in reverse order");
}
assert_throws(() => StyleText("neon", "x"), "RangeError", "unknown style is a RangeError");

const names = Styles();
assert_true(names.indexOf("bold") >= 0 && names.indexOf("bgBlue") >= 0 && names.indexOf("gray") >= 0,
  "Styles includes basics: " + JSON.stringify(names.slice(0, 5)));

// TTY queries: stdout is a file in probes -> not a TTY
assert_eq(IsTTY(1), false, "IsTTY(1) false for piped stdout");
assert_eq(typeof IsTTY(2), "boolean", "IsTTY returns boolean");
assert_true(Columns() > 0, "Columns positive");
assert_eq(ColorDepth(), 0, "ColorDepth 0 on non-TTY (documented)");
// NO_COLOR cannot make a non-TTY colored
setEnvSafe();
function setEnvSafe() { /* NO_COLOR path needs a TTY; documented, not assertable here */ }
summary("cli.style");
''')
print("gen_cli: 2 probes")
