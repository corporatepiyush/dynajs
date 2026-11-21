// covers: dyna:structures.InvertedIndex.tokenize
// covers: c:src/core/ds/ds-inverted-index.c
// The tokenizer classifies each byte through a 256-entry table (ASCII word,
// ASCII separator, non-ASCII slow path). A JS oracle re-implements the
// documented byte rules over the UTF-8 bytes; every byte value is tried as a
// joiner between two words, and random mixed text is compared token for token.
import { InvertedIndex } from "dyna:structures";
import { suite } from "../lib/kit.js";

const t = suite("test_structures_inverted_index_byte_classes");
const enc = new TextEncoder(), dec = new TextDecoder();
function oracle(text) {
    const s = enc.encode(text), n = s.length, out = [];
    const word = (i) => {
        const c = s[i];
        if (c < 0x80) return [/[A-Za-z0-9_]/.test(String.fromCharCode(c)), 1];
        if (c === 0xc2 && i + 1 < n && s[i + 1] >= 0xa0 && s[i + 1] <= 0xbf && s[i + 1] !== 0xaa && s[i + 1] !== 0xb5 && s[i + 1] !== 0xba) return [false, 2];
        if (c === 0xe2 && i + 2 < n && (s[i + 1] === 0x80 || s[i + 1] === 0x81)) return [false, 3];
        if (c === 0xe3 && i + 2 < n && s[i + 1] === 0x80) return [false, 3];
        if (c === 0xef && i + 2 < n && ((s[i + 1] === 0xbc && s[i + 2] >= 0x81 && s[i + 2] <= 0x8f) || (s[i + 1] === 0xbd && s[i + 2] >= 0x9b))) return [false, 3];
        return [true, 1];
    };
    let i = 0;
    while (i < n) {
        let w;
        while (i < n && !(w = word(i))[0]) i += w[1];
        const b = [];
        while (i < n && word(i)[0]) {
            const c = s[i++];
            b.push(c >= 65 && c <= 90 ? c + 32 : c);
        }
        if (b.length) out.push(dec.decode(new Uint8Array(b)));
    }
    return out;
}

t.test("every ASCII byte between two words splits or joins as the oracle says", ({ eq }) => {
    let bad = 0;
    for (let c = 0; c < 128; c++) {
        const text = "Foo" + String.fromCharCode(c) + "bAr";
        const got = JSON.stringify(InvertedIndex.tokenize(text));
        const exp = JSON.stringify(oracle(text));
        if (got !== exp && bad++ < 5) eq(got, exp, `byte 0x${c.toString(16)}`);
    }
    eq(bad, 0, "mismatches");
});

t.test("non-ASCII joiners and separators match the oracle", ({ eq }) => {
    let bad = 0;
    const cps = [];
    for (let c = 0x80; c < 0x800; c++) cps.push(c);
    for (const c of [0x2000, 0x200b, 0x2028, 0x2060, 0x2070, 0x3000, 0x3001, 0x3041, 0xff01, 0xff0f, 0xff10, 0xff5b, 0xff61, 0xffef, 0x1f600]) cps.push(c);
    for (const c of cps) {
        const text = "ab" + String.fromCodePoint(c) + "Cd";
        const got = JSON.stringify(InvertedIndex.tokenize(text));
        const exp = JSON.stringify(oracle(text));
        if (got !== exp && bad++ < 5) eq(got, exp, `U+${c.toString(16)}`);
    }
    eq(bad, 0, "mismatches");
});

t.test("random mixed text tokenizes like the oracle and is searchable", ({ eq }) => {
    let seed = 5;
    const rnd = (k) => {
        seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
        return (seed >>> 8) % k;
    };
    const alphabet = "abcXYZ019_ -.,;:!?'\"()[]{}<>/\\|@#$%^&*+=~`\t\né ß—　中！";
    let bad = 0;
    for (let d = 0; d < 300; d++) {
        let text = "";
        const len = 5 + rnd(60);
        for (let i = 0; i < len; i++) text += alphabet[rnd(alphabet.length)];
        const got = JSON.stringify(InvertedIndex.tokenize(text));
        const exp = JSON.stringify(oracle(text));
        if (got !== exp && bad++ < 5) eq(got, exp, JSON.stringify(text));
    }
    eq(bad, 0, "mismatches");
    const ix = new InvertedIndex();
    ix.add(1, "Alpha-beta gamma_delta");
    ix.add(2, "alpha;BETA");
    eq(JSON.stringify(ix.search("beta", { mode: "and" }).map((r) => r[0]).sort()), "[1,2]", "beta in both");
    eq(ix.docFreq("gamma_delta"), 1, "underscore joins");
});

await t.run();
