// Parametric black-box contract test for dyna:cli, generated from dynajs.d.ts lines 373-490. Engine sources not consulted.
import * as cli from "dyna:cli";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

// TTY-independent structural fact per the doc's auto-dim note: with escapes emitted OR dropped,
// stripping ANSI SGR/CSI sequences always leaves the original text.
const stripAnsi = (s) => s.replace(/\x1b\[[0-9;:]*[A-Za-z]/g, "");

// ============================ StyleText: accepted styles (d.ts L374-380) ============================
// "Styles text with the named ANSI style (or array of styles) ... Emission auto-dims ... Style NAMES are
//  validated either way." Only TTY-independent facts are asserted: the text survives (styled or plain).
{
  const cases = [
    ["style 'red'", "red", "hello"],
    ["style 'bold'", "bold", "hi"],
    ["dynamic form 256:5", "256:5", "x"],
    ["dynamic form bg256:200", "bg256:200", "x"],
    ["dynamic form #ff8800", "#ff8800", "x"],
    ["dynamic form bg#112233", "bg#112233", "x"],
    ["dynamic form rgb:1,2,3", "rgb:1,2,3", "x"],
    ["dynamic form bgRgb:10,20,30", "bgRgb:10,20,30", "x"],
    ["array of styles", ["red", "bold"], "x"],
    ["empty text", "red", ""],
  ];
  for (const [label, style, text] of cases) {
    const out = cli.StyleText(style, text);
    assertEq(stripAnsi(out), text, label + " delivers the text with escapes emitted or dropped");
  }
}

// ============================ StyleText: validation refusals (d.ts L379-380) ============================
// "an unknown one always throws" — TTY-independent validation side.
{
  const refusals = [
    ["unknown style name", "definitely-not-a-style"],
    ["256 out of range high", "256:300"],
    ["256 out of range negative", "256:-1"],
    ["256 non-numeric", "256:abc"],
    ["rgb too few components", "rgb:1,2"],
    ["rgb component out of range", "rgb:1,2,300"],
    ["hex too short", "#ff880"],
    ["hex non-hex digits", "#zzzzzz"],
    ["unknown style inside array", ["red", "bogus-name"]],
  ];
  for (const [label, style] of refusals) assertThrows(() => cli.StyleText(style, "x"), label);
}

// ============================ Styles (d.ts L381-382) ============================
// "The list of styles the engine can apply." — every listed style must be accepted by StyleText (parity).
{
  const styles = cli.Styles();
  assert(Array.isArray(styles) && styles.length >= 1, "Styles() is a non-empty array");
  for (const [i, s] of styles.entries()) {
    assert(typeof s === "string", "Styles()[" + i + "] is a string");
    assertEq(stripAnsi(cli.StyleText(s, "t")), "t", "listed style '" + s + "' is accepted by StyleText");
  }
}

// ============================ IsTTY / Columns / ColorDepth (d.ts L383-388) ============================
// Non-rendering facts only; no TTY-state assumptions in expectations.
{
  const cases = [
    ["IsTTY() default fd", () => cli.IsTTY()],
    ["IsTTY(1)", () => cli.IsTTY(1)],
    ["IsTTY(2)", () => cli.IsTTY(2)],
  ];
  for (const [label, fn] of cases) assert(typeof fn() === "boolean", label + " returns a boolean");
  assert(Number.isInteger(cli.Columns()) && cli.Columns() >= 0, "Columns() is a non-negative integer column count");
  const depthCases = [
    ["ColorDepth() default fd", () => cli.ColorDepth()],
    ["ColorDepth(1)", () => cli.ColorDepth(1)],
    ["ColorDepth(2)", () => cli.ColorDepth(2)],
  ];
  for (const [label, fn] of depthCases) {
    const d = fn();
    assert(d === 0 || d === 4 || d === 8 || d === 24, label + " is one of the documented depths {0, 4, 8, 24}");
  }
}

// ============================ prompt: strict options bag (d.ts L390-396) ============================
// "opts is a strict bag: null/undefined counts as absent, any other non-object throws." — the documented
// validation side only (no stdin interaction is driven).
{
  const refusals = [
    ["prompt string bag", () => cli.prompt("q", "not-an-object")],
    ["prompt number bag", () => cli.prompt("q", 42)],
    ["prompt boolean bag", () => cli.prompt("q", true)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ ProgressBar: STRICT ctor options bag (d.ts L424) ============================
// "constructor(opts: { total: number })" read with the module family's strict-bag doctrine
// (d.ts L450-453): "an unknown key throws a TypeError naming the key and the valid set. Only a
// null/undefined bag counts as absent; any other non-object bag throws."
{
  let msg = "";
  try { new cli.ProgressBar({ totl: 5 }); } catch (e) { msg = String(e); }
  assert(msg.includes("totl"), "an unknown ProgressBar key names the key, got |" + msg + "|");
  assert(msg.includes("total"), "and the valid set (total), got |" + msg + "|");
  const refusals = [
    ["non-object bag is refused", () => new cli.ProgressBar("x")],
    ["missing total is refused", () => new cli.ProgressBar({})],
    ["number bag is refused", () => new cli.ProgressBar(7)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ ProgressBar (d.ts L422-428) ============================
// "reaching total finishes the line and any later update is refused" — ordered step table, non-rendering facts.
{
  const pb = new cli.ProgressBar({ total: 10 });
  const steps = [
    ["update(0) returns this", () => pb.update(0) === pb],
    ["update(5) returns this", () => pb.update(5) === pb],
    ["update(total) returns this and finishes", () => pb.update(10) === pb],
  ];
  for (const [label, step] of steps) assert(step(), label);
  const refusals = [
    ["update after finish (same n)", () => pb.update(10)],
    ["update after finish (other n)", () => pb.update(3)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ Spinner (d.ts L430-438) ============================
// Frames "|/-\"; chaining returns this; non-rendering facts only.
{
  const cases = [
    ["default ctor + start/tick/stop chaining", () => {
      const s = new cli.Spinner();
      return s.start() === s && s.tick() === s && s.stop() === s;
    }],
    ["text ctor + stop(text) chaining", () => {
      const s = new cli.Spinner({ text: "working" });
      return s.start() === s && s.tick() === s && s.tick() === s && s.stop("done") === s;
    }],
  ];
  for (const [label, fn] of cases) assert(fn(), label);
}

// ============================ Table (d.ts L440-448) ============================
// "Renders rows as aligned columns ('grid'), TSV, or RFC 4180 CSV."
{
  const cases = [
    ["grid default contains cells", () => {
      const t = cli.Table([["alpha", "beta"]]);
      return typeof t === "string" && t.includes("alpha") && t.includes("beta");
    }],
    ["number cells render", () => cli.Table([[1, 2]]).includes("1")],
    ["tsv exact rows", () => {
      const lines = cli.Table([["a", "b"], ["c", "d"]], { format: "tsv" }).split("\n").filter((l) => l !== "");
      return eqArr(lines, ["a\tb", "c\td"]);
    }],
    ["csv comma-joins rows", () => {
      const t = cli.Table([["a", "b"], ["c", "d"]], { format: "csv" });
      return t.includes("a,b") && t.includes("c,d");
    }],
    ["csv RFC 4180 quotes embedded commas", () => cli.Table([["x,y"]], { format: "csv" }).includes('"x,y"')],
    ["head row rendered first", () => {
      const t = cli.Table([["r1c1"]], { head: ["H1"] });
      return t.indexOf("H1") !== -1 && t.indexOf("H1") < t.indexOf("r1c1");
    }],
    ["align option accepted", () => cli.Table([["a"]], { align: ["right"] }).includes("a")],
  ];
  for (const [label, fn] of cases) assert(fn(), label);
}

// ============================ Command: construction and chaining (d.ts L454-489) ============================
{
  const c = new cli.Command("prog");
  assertEq(c.name, "prog", "constructor name is exposed via get name");
  assertEq(c.version(), "", "version() is '' when unset"); // documented: '"" when unset'
  assertEq(c.version("2.0.1"), c, "version(v) is chainable (returns this)");
  assertEq(c.version(), "2.0.1", "version() getter returns the set version");
  const chainCases = [
    ["describe returns this", (x) => x.describe("a test command") === x],
    ["option returns this", (x) => x.option("--num", "a number", { type: "number" }) === x],
    ["argument returns this", (x) => x.argument("file", "an input") === x],
    ["allowUnknown returns this", (x) => x.allowUnknown(false) === x],
    ["action returns this", (x) => x.action(() => 0) === x],
  ];
  for (const [label, step] of chainCases) assertEq(step(c), true, label);
}

// ============================ Command: STRICT options bags (d.ts L450-453, L467) ============================
// "Every options bag is STRICT: an unknown key throws a TypeError naming the key and the valid set.
//  Only a null/undefined bag counts as absent; any other non-object bag throws."
{
  const refusals = [
    ["option unknown key names the key", () => new cli.Command("t").option("--x", "d", { tipe: "number" }), /tipe/],
    ["option non-object bag", () => new cli.Command("t").option("--x", "d", "nope"), undefined],
  ];
  for (const [label, fn, pat] of refusals) assertThrows(fn, label, TypeError, pat);
  // null bag counts as absent (documented) — accepted, registers the option
  assertEq(new cli.Command("t").option("--x", "d", null) !== undefined, true, "option with null bag counts as absent");
}

// ============================ Command: strict finite-decimal number grammar (d.ts L463-466) ============================
// "'8', '-.5', '1e3'; surrounding ASCII whitespace ignored'; 'NaN', 'Infinity' and '0x10' are refused."
{
  const c = new cli.Command("t");
  c.option("--num", "d", { type: "number" });
  const cases = [
    ["'8'", "8", 8],
    ["'-42'", "-42", -42],
    ["'-.5'", "-.5", -0.5],
    ["'1e3'", "1e3", 1000],
    ["'0.5'", "0.5", 0.5],
    ["' 8 ' whitespace ignored", " 8 ", 8],
  ];
  for (const [label, raw, expected] of cases) assertEq(c.parse(["--num", raw]).options.num, expected, "number option " + label);
  const refusals = [
    ["'NaN' refused", "NaN"],
    ["'Infinity' refused", "Infinity"],
    ["'0x10' refused", "0x10"],
    ["'abc' refused", "abc"],
  ];
  for (const [label, raw] of refusals) assertThrows(() => c.parse(["--num", raw]), "number option " + label);
}

// ============================ Command: boolean option + default + precedence (d.ts L467, L484-485) ============================
// "Each option resolves CLI-first: a CLI-supplied option uses its parsed value(s) ... otherwise env, then default, apply."
{
  const c = new cli.Command("t");
  c.option("--verbose", "d", { type: "boolean" });
  c.option("--num", "d", { type: "number", default: 5 });
  assertEq(c.parse(["--verbose"]).options.verbose, true, "boolean option set by its flag");
  assertEq(c.parse([]).options.num, 5, "default applies when the CLI does not supply the option");
  assertEq(c.parse(["--num", "9"]).options.num, 9, "CLI beats default");
}

// ============================ Command: unknown options (d.ts L472-473) ============================
// "Permits unknown options instead of refusing them." — default is refusal.
{
  const c = new cli.Command("t");
  c.option("--num", "d", { type: "number" });
  assertThrows(() => c.parse(["--zzz"]), "unknown option refused by default");
  const c2 = new cli.Command("t");
  c2.option("--num", "d", { type: "number" });
  c2.allowUnknown(true);
  assert(typeof c2.parse(["--zzz"]) === "object", "allowUnknown(true) permits unknown options");
}

// ============================ Command: parse result shape (d.ts L481-486) ============================
// "Returns { options, arguments, command } ... command: string | null."
{
  const c = new cli.Command("t");
  const r = c.parse([]);
  assert(r !== null && typeof r.options === "object", "parse result has an options object");
  assert(Array.isArray(r.arguments), "parse result has an arguments array");
  assertEq(r.command, null, "command is null with no subcommand");
  assert(eqArr(c.parse(["a", "b"]).arguments, ["a", "b"]), "positional arguments are collected in order");
}

// ============================ Command: action + subcommands (d.ts L470-476) ============================
// "Registers the handler a leaf parse invokes with (options, args); its return value lands on the parse result's result."
{
  const sub = new cli.Command("sub");
  sub.action(() => "S");
  const root = new cli.Command("root");
  assertEq(root.command(sub), root, "command() chains");
  const r = root.parse(["sub"]);
  assertEq(r.command, "sub", "parse result names the chosen subcommand");
  // DOC-TENSION: action()'s doc says the return "lands on the parse result's
  // result" without saying how subcommand layers compose; the engine lands the
  // LEAF's whole parse result ({options, arguments, command, result}) on the
  // parent's `result`, so the leaf return sits one level deeper. A ROOT
  // action's return sits directly on .result (next row).
  assertEq(r.result.result, "S", "leaf action return lands on parse result.result.result");
  const root2 = new cli.Command("t");
  root2.action((options, args) => args.length);
  const r2 = root2.parse(["x", "y"]);
  assertEq(r2.result, 2, "root action receives (options, args)");
}

// ============================ Command: help (d.ts L487-488) ============================
// "Prints the help text."
{
  const c = new cli.Command("t");
  assert(typeof c.help() === "string", "help() returns a string");
  const described = new cli.Command("prog").describe("does things");
  assert(described.help().length > 0, "help() of a described command is non-empty");
}

/* ------------------------------------------------------------------ *
 *  Interactive surface (d.ts L390-420), ported from tests/test_term.js
 *  withIO rows. std/os are the --std modules (d.ts L6700: "available
 *  with --std") and this suite must run flagless, so the rows execute
 *  in a self-spawned child (dyna:sys Exec + args()[0], both documented;
 *  the child script is written to a temp dir at runtime) and this file
 *  parses the child's ROW-OK/ROW-FAIL protocol.
 * ------------------------------------------------------------------ */
import { Exec, args, Spawn } from "dyna:sys";
import { makeTempDir, removeAll, writeFile, Path } from "dyna:file";

// Child script 1: withIO harness (fed stdin strings/bytes via a dup2'd
// file, stdout captured) + the representative prompt/confirm/select
// rows and the byte-level keypress rules.
const CHILD_TERM = String.raw`
import * as std from "std";
import * as os from "os";
import * as cli from "dyna:cli";

// Ported verbatim-in-shape from tests/test_term.js withIO: feeds stdin
// bytes and captures stdout around one synchronous call.
function withIO(data, fn) {
    std.out.flush();
    const pin = "/tmp/bb_cli_io_" + os.getpid() + ".in";
    const pout = "/tmp/bb_cli_io_" + os.getpid() + ".out";
    if (typeof data === "string") {
        const w = std.open(pin, "w"); w.puts(data); w.close();
    } else {
        const fd = os.open(pin, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644);
        os.write(fd, new Uint8Array(data).buffer);
        os.close(fd);
    }
    const fo = std.open(pout, "w");
    const fi = std.open(pin, "r");
    const sin = os.dup(0), sout = os.dup(1);
    os.dup2(fi.fileno(), 0);
    os.dup2(fo.fileno(), 1);
    let r, err;
    try { r = fn(); } catch (e) { err = e; }
    std.out.flush();
    os.dup2(sin, 0); os.dup2(sout, 1);
    os.close(sin); os.close(sout); fi.close(); fo.close();
    const rf = std.open(pout, "r"); const out = rf.readAsString(); rf.close();
    os.remove(pin); os.remove(pout);
    if (err) throw err;
    return { result: r, out: out };
}
function keys(inp, n) {
    return withIO(inp, () => {
        const out = [];
        for (let i = 0; i < n; i++) {
            const k = cli.keypress();
            if (k === null) { out.push("EOF"); break; }
            out.push(k);
        }
        return out;
    }).result;
}
let fails = 0;
function eq(got, want, msg) {
    if (got !== want) { fails++; print("ROW-DETAIL " + msg + " (got " + JSON.stringify(got) + ", want " + JSON.stringify(want) + ")"); }
}
function row(name, fn) {
    try { fn(); print("ROW-OK " + name); }
    catch (e) { fails++; print("ROW-FAIL " + name + ": " + (e && e.message ? e.message : String(e))); }
}
const MIB = 1 << 20;

// d.ts L390-396: "Writes message to stdout and reads one line from
// stdin" / "The terminator and a CRLF's CR are dropped first".
row("prompt: echo + message verbatim + CR drop", () => {
    const r = withIO("hello world\n", () => cli.prompt("Q: "));
    eq(r.result, "hello world", "prompt reads the line");
    eq(r.out, "Q: ", "the message is written verbatim");
    eq(withIO("x\r\n", () => cli.prompt("Q")).result, "x", "CRLF loses the CR");
});
// d.ts L390-391: "An empty answer, or EOF, yields opts.default (or ''
// without one)."
row("prompt: default on empty answer and on EOF", () => {
    eq(withIO("\n", () => cli.prompt("Q", { default: "D" })).result, "D", "empty answer takes the default");
    eq(withIO("", () => cli.prompt("Q", { default: "D" })).result, "D", "EOF takes the default");
    eq(withIO("", () => cli.prompt("Q")).result, "", "EOF without a default is the empty string");
});
// d.ts L392-393: "an answer longer than 1 MiB (counted after that drop)
// throws RangeError".
row("prompt: 1 MiB input cap", () => {
    let cls = "";
    withIO("a".repeat(MIB + 2), () => { try { cli.prompt("Q"); } catch (e) { cls = e.constructor.name; } });
    eq(cls, "RangeError", "past 1 MiB is a RangeError");
    eq(withIO("a".repeat(MIB) + "\n", () => cli.prompt("Q")).result.length, MIB, "exactly 1 MiB is accepted");
});
// d.ts L397-399: "'y'/'yes' (ASCII case-insensitive) answers true,
// everything else (including EOF) false."
row("confirm: parse table", () => {
    const cases = [["y\n", true], ["Y\n", true], ["yes\n", true], ["YES\n", true],
                   ["yes\r\n", true], ["y", true],
                   ["n\n", false], ["\n", false], ["maybe\n", false], ["", false],
                   [" y\n", false], ["yeah\n", false]];
    for (const [inp, want] of cases)
        eq(withIO(inp, () => cli.confirm("c?")).result, want, "confirm " + JSON.stringify(inp));
});
// d.ts L400-403: "returns the chosen option string, or null when
// cancelled (Escape, Ctrl-C) or stdin reaches EOF."
row("select: index bounds and cancels", () => {
    eq(withIO("\r", () => cli.select("pick", ["one", "two"])).result, "one", "enter takes the highlighted option");
    eq(withIO("\x1b[B\r", () => cli.select("m", ["one", "two"])).result, "two", "down moves and enter takes");
    eq(withIO("\x1b[B\x1b[B\r", () => cli.select("m", ["one", "two"])).result, "one", "down wraps forward");
    eq(withIO("\x1b[A\r", () => cli.select("m", ["one", "two"])).result, "two", "up wraps backward");
    eq(withIO("\x1b", () => cli.select("m", ["one", "two"])).result, null, "escape cancels");
    eq(withIO("\x03", () => cli.select("m", ["one", "two"])).result, null, "ctrl-c cancels");
    eq(withIO("", () => cli.select("m", ["one", "two"])).result, null, "EOF cancels");
});
// d.ts L404-407: "input past the 15-byte sequence cap is delivered as
// the NEXT event's input."
row("keypress: 15-byte cap carries the tail", () => {
    const ks = keys(new Array(20).fill(0x1B), 4);
    eq(ks.length, 3, "two events then EOF");
    eq(ks[0].name, "escape", "the head is an escape event");
    eq(ks[0].sequence.length, 15, "which reports exactly the 15 bytes it consumed");
    eq(ks[0].meta, true, "an ESC run is the meta chord");
    eq(ks[1].name, "escape", "the tail is delivered as its own event");
    eq(ks[1].sequence.length, 5, "5 more bytes: 20 consumed, 20 reported");
    eq(ks[2], "EOF", "then EOF");
});
// d.ts L405-407: "a byte that cannot extend the current event (a
// non-continuation after a truncated UTF-8 lead ...) is delivered as
// the NEXT event's input"; L414-415 "malformed runs appear as U+FFFD".
row("keypress: truncated UTF-8 lead is U+FFFD and carries on", () => {
    const ks = keys([0xC3, 0x41, 0x42], 4);
    eq(ks[0].name, null, "the broken lead is an unknown key");
    eq(ks[0].sequence, "\uFFFD", "reported as U+FFFD");
    eq(ks[1].name, "A", "the byte after the broken lead is delivered, not swallowed");
    eq(ks[2].name, "B", "and the stream continues unharmed");
    eq(ks[3], "EOF", "with nothing lost and nothing extra");
});
// d.ts L408-410: "identical bytes decode identically while the gaps
// between them stay inside it [the 50 ms quiet window]" — both bursts
// arrive from one file feed, so every gap is ~0 ms, inside the window.
row("keypress: identical bursts inside the window decode identically", () => {
    const ks = keys("\x1b[A\x1b[A", 4);
    eq(ks.length, 3, "two events then EOF");
    eq(ks[0].name, "up", "the first burst decodes up");
    eq(ks[1].name, "up", "the second identical burst decodes identically");
    eq(ks[0].sequence, ks[1].sequence, "with the same sequence");
});

print(fails ? "CHILD-FAIL " + fails : "CHILD-ALL-OK");
std.out.flush();
if (fails) std.exit(1);
`;

// Child script 2: prints keypress() events as one JSON EVT line each —
// stdin is a PIPE written by this suite with real timing, which is how
// the >50 ms side of the quiet-window rule is exercised flaglessly.
const CHILD_KEYS = String.raw`
import * as cli from "dyna:cli";
const want = parseInt(scriptArgs.filter((a) => /^\d+$/.test(a))[0], 10) || 3;
for (let i = 0; i < want; i++) {
    const k = cli.keypress();
    print("EVT " + JSON.stringify(k));
    if (k === null) break;
}
print("CHILD2-DONE");
`;

async function readSpawnPipe(src) {
  const buf = new Uint8Array(65536);
  let out = "";
  for (;;) {
    const k = await src.read(buf);
    if (k === 0) break;
    out += new TextDecoder().decode(buf.subarray(0, k));
  }
  return out;
}

{
  const tmp = makeTempDir("bbcli-");
  const child1 = tmp + "/child_term.js";
  const child2 = tmp + "/child_keys.js";
  try {
    writeFile(new Path(child1), CHILD_TERM);
    writeFile(new Path(child2), CHILD_KEYS);

    // --- ITEMS via withIO child: prompt/confirm/select + byte rules ---
    const r = Exec(args()[0], ["--std", child1], { timeoutMs: 30000 });
    assertEq(r.code, 0, "interactive child exits 0");
    assert(r.stdout.includes("CHILD-ALL-OK"), "interactive child reports success, got |" + r.stdout.slice(-400) + "|");
    // surface any per-row detail and count each passing row
    for (const line of r.stdout.split("\n")) {
      if (line.startsWith("ROW-FAIL") || line.startsWith("ROW-DETAIL")) print(line);
    }
    const okRows = (r.stdout.match(/^ROW-OK /gm) || []).length;
    assertEq(okRows, 8, "every interactive child row ran (prompt/confirm/select/keypress)");
    n += okRows;

    // --- keypress 50 ms quiet window, >50 ms side (d.ts L408-409) ---
    // "a lone ESC is told apart from a sequence head only by a 50 ms
    // quiet window": write one ESC, stay quiet 150 ms (> window), then
    // a fresh burst. The ESC must decode as a LONE escape (meta unset)
    // that consumed nothing of the later burst.
    const sp = new Spawn(args()[0], ["--std", child2, "3"], { stdin: "pipe" });
    await sp.stdin.write(new Uint8Array([0x1b]));
    await sp.stdin.flush();
    await sleep(150);
    await sp.stdin.write(new Uint8Array([0x1b, 0x5b, 0x41])); // ESC [ A
    await sp.stdin.flush();
    await sleep(150);
    sp.stdin.close();
    const [out2, , res2] = await Promise.all([readSpawnPipe(sp.stdout), readSpawnPipe(sp.stderr), sp.wait()]);
    assertEq(res2.code, 0, "keypress pipe child exits 0");
    const evts = out2.split("\n").filter((l) => l.startsWith("EVT "))
      .map((l) => JSON.parse(l.slice(4)));
    assertEq(evts.length, 3, "three keypress events reported");
    assertEq(evts[0].name, "escape", "an ESC quiet past the 50 ms window decodes as lone escape");
    assertEq(evts[0].meta, false, "lone escape is not the meta chord");
    assertEq(evts[0].sequence, "\x1b", "its sequence is exactly the one byte");
    assertEq(evts[1].name, "up", "the later burst decodes as its own event (nothing swallowed)");
    assertEq(evts[1].sequence, "\x1b[A", "the burst's full sequence");
    assertEq(evts[2], null, "then EOF");
    n += 8;
  } finally {
    removeAll(tmp);
  }
}

print("bb_cli: all tests passed (" + n + " assertions)");
