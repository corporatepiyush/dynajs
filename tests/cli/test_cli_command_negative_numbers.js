// covers: c:src/modules/term/term-style.c
// Command.parse treats an argument that starts with "-" as a positional value when the whole of it
// is a number to strtod (decimal, exponent, hex float, inf, infinity, nan, nan(...)). It now
// refuses at once when the byte after the sign is not a digit, '.', 'i'/'I' or 'n'/'N', which no
// strtod number can follow a sign with; strtod was ~8% of parsing option-heavy argv. The oracle
// is a JS model of strtod's full-match grammar; over every token built from "-" plus 1..3
// characters of an alphabet of digits, '.', exponent and hex letters and the option letters, a
// number must land in arguments with no option set (also without allowUnknown, where an unknown
// flag such as "-." is an error), and a non-number made only of declared short flags must set them
// and leave arguments empty.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_command_negative_numbers");
import { Command } from "dyna:cli";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };
const isNum = (t) => /^[+-]?((\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?|inf|infinity|nan(\([0-9A-Za-z_]*\))?|0[xX]([0-9a-fA-F]+(\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)([pP][+-]?\d+)?)$/i.test(t);
const SHORTS = "aefinpxtyIN";
const mk = (strict) => {
    const c = new Command("t").allowUnknown(!strict).argument("rest...", "r");
    for (const ch of SHORTS) c.option("-" + ch + ", --opt-" + ch.charCodeAt(0), "o", { type: "boolean" });
    return c;
};

function check(t) {
    const r = mk().parse([t]);
    const set = Object.keys(r.options).filter((k) => r.options[k] === true);
    if (isNum(t)) {
        eq(JSON.stringify(r.arguments), JSON.stringify([t]), "number " + JSON.stringify(t) + " is positional");
        eq(set.length, 0, "number " + JSON.stringify(t) + " sets no option");
        let strict;
        try { strict = JSON.stringify(mk(true).parse([t]).arguments); } catch (e) { strict = e.message; }
        eq(strict, JSON.stringify([t]), "number " + JSON.stringify(t) + " is positional without allowUnknown");
    } else if (/^-[a-zA-Z]+$/.test(t) && [...t.slice(1)].every((ch) => SHORTS.includes(ch))) {
        eq(r.arguments.length, 0, "flags " + JSON.stringify(t) + " are not positional");
        eq(set.length, new Set(t.slice(1)).size, "flags " + JSON.stringify(t) + " set each option");
    }
}

__kitS.section("every short token over the number and flag alphabet", () => {
    const A = "0159.eEinfaxpINF+-()";
    let count = 0;
    const walk = (s, d) => {
        if (d > 0) { check("-" + s); count++; }
        if (d < 3) for (const ch of A) walk(s + ch, d + 1);
    };
    walk("", 0);
    eq(count > 8000, true, "walked " + count + " tokens");
});

__kitS.section("named numbers and near misses", () => {
    for (const t of ["-5", "-0", "-.5", "-5.", "-1e3", "-1E+3", "-1e-3", "-0x10", "-0X1p3", "-0x.8", "-inf", "-INF", "-Infinity", "-infinity",
        "-nan", "-NaN", "-nan(abc)", "-nan()", "-.", "-e5", "-x1", "-in", "-infx", "-nanx", "-1e", "-0x", "-", "--", "-i", "-n", "-I", "-N", "-a", "-ef"])
        check(t);
    const r = mk().parse(["-5", "-i", "-.25", "-nan", "-ap", "-1e2"]);
    eq(JSON.stringify(r.arguments), JSON.stringify(["-5", "-.25", "-nan", "-1e2"]), "a mixed argv keeps numbers in order");
});

__kitS.end();
print(`test_cli_command_negative_numbers: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
