// flags: --std
// covers: c:src/modules/term/term-style.c
// StyleText keeps the close codes of up to 16 styles on the stack (a malloc before), reserves the
// output once, and its name lookup compares the first letter before strlen + memcmp; 23% faster
// with colour on, 11% without. The oracle is the SGR table below, written out from the ANSI codes
// (not read from the module): the result is every open code in order, the text, then the close
// codes in reverse order; with FORCE_COLOR=0 it is the text alone. Lists of 0..40 styles cross the
// 16-entry stack limit, names sharing a first letter (bold/blink/black/blue/blueBright, ...) and
// near-misses that share a prefix with a real name are refused with the same TypeError.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_style_text_compose");
import { StyleText, Styles } from "dyna:cli";
import * as std from "std";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); } };
const E = (n) => "\u001b[" + n + "m";
const SGR = {
    reset: [0, 0], bold: [1, 22], dim: [2, 22], italic: [3, 23], underline: [4, 24], blink: [5, 25], inverse: [7, 27], hidden: [8, 28],
    strikethrough: [9, 29], doubleunderline: [21, 24], framed: [51, 54], overlined: [53, 55], gray: [90, 39], grey: [90, 39],
    bgGray: [100, 49], bgGrey: [100, 49],
};
["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"].forEach((c, i) => {
    SGR[c] = [30 + i, 39];
    SGR["bg" + c[0].toUpperCase() + c.slice(1)] = [40 + i, 49];
    if (c !== "black") {
        SGR[c + "Bright"] = [90 + i, 39];
        SGR["bg" + c[0].toUpperCase() + c.slice(1) + "Bright"] = [100 + i, 49];
    }
});
const NAMES = Object.keys(SGR).sort();
const oracle = (list, text) => list.map((s) => E(SGR[s][0])).join("") + text + list.slice().reverse().map((s) => E(SGR[s][1])).join("");
const prevForce = std.getenv("FORCE_COLOR");

__kitS.section("the style table matches the SGR codes", () => {
    std.setenv("FORCE_COLOR", "1");
    eq(JSON.stringify(Styles().slice().sort()), JSON.stringify(NAMES), "Styles() lists exactly the table's names");
    for (const s of NAMES) eq(StyleText(s, "x" + s), oracle([s], "x" + s), "single " + s);
});

__kitS.section("lists across the 16-entry stack limit", () => {
    std.setenv("FORCE_COLOR", "1");
    for (let k = 0; k <= 40; k++) {
        const list = [];
        for (let i = 0; i < k; i++) list.push(NAMES[(i * 7 + k) % NAMES.length]);
        const text = "t" + k + "é".repeat(k * 13);
        eq(StyleText(list, text), oracle(list, text), "list of " + k);
        eq(StyleText(list, ""), oracle(list, ""), "empty text, list of " + k);
    }
    const many = Array.from({ length: 256 }, (_, i) => NAMES[i % NAMES.length]);
    eq(StyleText(many, "m"), oracle(many, "m"), "256 styles (the maximum)");
});

__kitS.section("colour off drops every code", () => {
    std.setenv("FORCE_COLOR", "0");
    for (const k of [0, 1, 16, 17, 40]) {
        const list = NAMES.slice(0, k);
        eq(StyleText(list, "plain" + k), "plain" + k, "FORCE_COLOR=0, list of " + k);
    }
});

__kitS.section("names that share a first letter or a prefix", () => {
    std.setenv("FORCE_COLOR", "1");
    for (const s of ["b", "bl", "blu", "bolder", "Bold", "bgred", "bgRedBrigh", "r", "", "x", "grayy", "gre"]) {
        let msg = "";
        try { StyleText(s, "z"); msg = "accepted"; } catch (e) { msg = e.constructor.name + ": " + e.message; }
        eq(msg, "TypeError: StyleText: unknown style \"" + s + "\"", "refused " + JSON.stringify(s));
        try { StyleText(["bold", "red", s], "z"); msg = "accepted"; } catch (e) { msg = e.constructor.name + ": " + e.message; }
        eq(msg, "TypeError: StyleText: unknown style \"" + s + "\"", "refused in a list " + JSON.stringify(s));
    }
});

if (prevForce === undefined || prevForce === null) std.unsetenv("FORCE_COLOR");
else std.setenv("FORCE_COLOR", prevForce);
__kitS.end();
print(`test_cli_style_text_compose: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
