function bench(name, f) {
    for (let i = 0; i < 3; i++) f();
    let best = Infinity;
    for (let r = 0; r < 7; r++) {
        const t0 = performance.now(); f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    console.log(name.padEnd(42) + best.toFixed(3) + " ms");
    return best;
}

const UNIT = "the quick brown fox jumps over the lazy dog ";
let hay = UNIT.repeat(4000);
const hayHit = hay + "NEEDLE_XYZ";
let wide = "naïve café — ünïcødé 日本語 ".repeat(4000);
const wideHit = wide + "☃ZZ";

console.log("--- floor: the same scan with no regex engine attached ---");
const floorLit = bench("indexOf('NEEDLE_XYZ')", () => hayHit.indexOf("NEEDLE_XYZ"));
const floorChr = bench("indexOf('~')  (absent)", () => hay.indexOf("~"));

console.log("--- LIT: multi-char literal prefix ---");
bench("/NEEDLE_XYZ/ hit at end", () => /NEEDLE_XYZ/.exec(hayHit));
bench("/NEEDLE_XYZ/ no match", () => /NEEDLE_XYZ/.exec(hay));
bench("/needle\\d+/ no match", () => /needle\d+/.exec(hay));
bench("/quick brown/g count", () => hay.match(/quick brown/g).length);

console.log("--- CHAR: single literal first char ---");
bench("/~/ absent (8-bit)", () => /~/.exec(hay));
bench("/z(?=ebra)/ rare first char", () => /z(?=ebra)/.exec(hay));
bench("/\\u2603ZZ/ absent (16-bit)", () => /☃ZZ/.exec(wide));
bench("/\\u2603ZZ/ hit (16-bit)", () => /☃ZZ/.exec(wideHit));

console.log("--- SET: small leading char class ---");
bench("/[0-9]{4}-[0-9]{2}/ no match", () => /[0-9]{4}-[0-9]{2}/.exec(hay));
bench("/[~^]/ absent", () => /[~^]/.exec(hay));

console.log("--- NONE: must not be prefiltered (guard rails) ---");
bench("/(foo|NEEDLE_XYZ)/ alternation", () => /(foo|NEEDLE_XYZ)/.exec(hayHit));
bench("/^NEEDLE_XYZ/ anchored", () => /^NEEDLE_XYZ/.exec(hayHit));
bench("/\\bNEEDLE_XYZ\\b/ word bound", () => /\bNEEDLE_XYZ\b/.exec(hayHit));

console.log("--- FOLD: ignore-case, IS prefiltered (was mislabelled NONE) ---");
bench("/NEEDLE_XYZ/i one exec", () => /NEEDLE_XYZ/i.exec(hayHit));
bench("/NEEDLE_XYZ/gi match-all", () => (hayHit.match(/NEEDLE_XYZ/gi) || []).length);
bench("/[q-t]uick/gi range_i /g", () => (hay.match(/[q-t]uick/gi) || []).length);
bench("/[wxyz]ebra/gi range_i absent", () => (hay.match(/[wxyz]ebra/gi) || []).length);
bench("/Q/gi char_i dense", () => (hay.match(/Q/gi) || []).length);
bench("/[q-t]uick/g  CONTROL", () => (hay.match(/[q-t]uick/g) || []).length);
bench("/Q/g  CONTROL", () => (hay.match(/Q/g) || []).length);

console.log("--- realistic: log/HTML scanning ---");
const log = ("127.0.0.1 - - [26/Jul/2026:10:00:00] \"GET /a/b HTTP/1.1\" 200 1234\n").repeat(3000);
bench("log: /\"GET [^\"]*\"/g", () => log.match(/"GET [^"]*"/g).length);
bench("log: /\\d+\\.\\d+\\.\\d+\\.\\d+/g", () => log.match(/\d+\.\d+\.\d+\.\d+/g).length);
const html = ("<div class='x'>text</div><p>more</p>").repeat(3000);
bench("html: /<\\/div>/g", () => html.match(/<\/div>/g).length);

console.log("\nfloor (indexOf literal): " + floorLit.toFixed(3) + " ms   " +
            "floor (indexOf char): " + floorChr.toFixed(3) + " ms");
