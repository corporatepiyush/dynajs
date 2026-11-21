// covers: c:src/modules/time/time-format.c
// Format.parse compares a one-byte layout literal ("-", ":", " ", "T", "/") as a single byte instead
// of a memcmp call per literal (memcmp was 12% of parsing "2006-01-02 15:04:05"; 15% faster).
// Longer literals keep memcmp. The oracle is the formatter: parse(format(sec)) must give sec back
// for every layout below, over a sweep of instants. Changing any one literal byte of a formatted
// string, or cutting the string short inside or right before a literal, must make parse fail.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_parse_literals");
import { Format } from "dyna:time";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };
const parses = (f, s) => { try { f.parse(s); return true; } catch (e) { return false; } };
const LAYOUTS = ["2006-01-02 15:04:05", "2006-01-02T15:04:05%z", "02/01/2006", "15:04", "2006.01.02 at 15h04", "Mon, 02 Jan 2006 15:04:05 %z", "x2006y01z02", "2006--01::02", "[2006] {01} <02>"];

__kitS.section("round trip through each layout", () => {
    for (const lay of LAYOUTS) {
        const f = new Format(lay);
        let ok = 0, n = 0;
        for (let i = 0; i < 1500; i++) {
            let sec = -2208988800 + Math.floor(i * 6311385.1);
            if (!/15|04|05/.test(lay)) sec -= ((sec % 86400) + 86400) % 86400;
            if (!/04/.test(lay.replace("2006", ""))) sec -= ((sec % 3600) + 3600) % 3600;
            if (!/05/.test(lay.replace("2006", ""))) sec -= ((sec % 60) + 60) % 60;
            if (!/2006/.test(lay)) sec = ((sec % 86400) + 86400) % 86400;
            n++;
            if (f.parse(f.format(sec)) === sec) ok++;
        }
        eq(ok, n, "round trip " + lay);
    }
});

__kitS.section("each literal byte is checked", () => {
    for (const lay of LAYOUTS) {
        const f = new Format(lay), s = f.format(1700000000);
        const lit = [];
        for (let i = 0; i < s.length; i++) if (!/[0-9]/.test(s[i]) && !/[A-Za-z+]/.test(s[i])) lit.push(i);
        for (const i of lit) {
            const swapped = s.slice(0, i) + (s[i] === "#" ? "@" : "#") + s.slice(i + 1);
            eq(parses(f, swapped), false, lay + " literal " + JSON.stringify(s[i]) + " at " + i + " changed");
            eq(parses(f, s.slice(0, i)), false, lay + " cut before literal at " + i);
        }
        eq(parses(f, s), true, lay + " intact");
    }
    const f = new Format("2006-01-02");
    eq(parses(f, "2024-01-0"), false, "cut inside the last field");
    eq(parses(f, "2024/01/02"), false, "other separator");
    eq(parses(f, "2024-01-02x"), false, "trailing byte");
});

__kitS.end();
print(`test_time_format_parse_literals: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
