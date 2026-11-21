// flags: --std
// covers: c:src/builtins/builtin-regexp.c
// Compiling a pattern consults a bounded per-context cache keyed by (pattern text, flags mask) and
// shares the immutable program. A program must never be shared with a different mask: the oracle
// derives each regexp's behaviour and its .flags (read back from the program) from the mask alone.
// Distinct patterns must not grow memory, and a sliced pattern must not keep its parent alive.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_regexp_compile_cache");
import * as std from "std";
import { memoryUsage } from "dyna:sys";

let n = 0,
    fails = 0;
const ok = (got, want, what) => {
    n++;
    const g = JSON.stringify(got),
        w = JSON.stringify(want);
    if (g !== w) {
        fails++;
        if (fails <= 20) print("FAIL: " + what + " got " + g + " want " + w);
    }
};

const LETTERS = ["d", "g", "i", "m", "s", "u", "v", "y"];
const masks = [];
for (let m = 0; m < 256; m++) if (!((m & 32) && (m & 64))) masks.push(m);
const flagStr = (m) => LETTERS.filter((_, i) => m & (1 << i)).join("");
const shuffled = (m) => flagStr(m).split("").reverse().join("");
const SUBJ = ["abc", "ABC", "a\nc", "x\nabc", "😀"];
const expectFor = (m) => {
    const i = !!(m & 4),
        mm = !!(m & 8),
        s = !!(m & 16),
        u = !!(m & 96);
    return [true, i, s || mm, mm, u, !!(m & 1)];
};
const observe = (re) => {
    const r = SUBJ.map((x) => {
        re.lastIndex = 0;
        return re.test(x);
    });
    re.lastIndex = 0;
    r.push(!!re.exec("abc").indices);
    return r;
};
const P = "^a.c$|^.$";
let bad = 0;
for (let round = 0; round < 3; round++) {
    const order = round === 1 ? [...masks].reverse() : round === 2 ? masks.map((m, k) => masks[(k * 37) % masks.length]) : masks;
    for (const m of order) {
        const re = new RegExp(P, round === 2 ? shuffled(m) : flagStr(m));
        const got = [re.flags, observe(re)];
        const want = [flagStr(m), expectFor(m)];
        if (JSON.stringify(got) !== JSON.stringify(want)) {
            bad++;
            if (bad <= 5) ok(got, want, "round " + round + " flags " + flagStr(m));
        }
    }
}
ok(bad, 0, "same pattern under all 192 flag masks, three orders: behaviour and .flags follow the mask");

__kitS.section("distinct patterns sharing slots each keep their own program", () => {
    let wrong = 0;
    for (let i = 0; i < 3000; i++) {
        const re = new RegExp("^x" + i + "$");
        if (!re.test("x" + i) || re.test("x" + (i + 1)) || re.source !== "^x" + i + "$") wrong++;
        const again = new RegExp("^x" + (i >> 1) + "$", "i");
        if (!again.test("X" + (i >> 1))) wrong++;
    }
    ok(wrong, 0, "distinct patterns sharing slots each keep their own program");
});
__kitS.section("wide patterns, u and non-u", () => {
    const a = new RegExp("ξ+", "u"),
        b = new RegExp("ξ+"),
        c = new RegExp("\\u{3be}", "u"),
        d = new RegExp("\\u{3be}");
    ok([a.test("ξξ"), b.test("ξξ"), c.test("ξ"), d.test("ξ"), d.test("u{3be}")], [true, true, true, false, true], "wide patterns, u and non-u");
});
__kitS.section("a syntax error is not cached as a success", () => {
    let threw = 0;
    for (let k = 0; k < 2; k++)
        try {
            new RegExp("(", "");
        } catch (e) {
            if (e instanceof SyntaxError) threw++;
        }
    ok(threw, 2, "a syntax error is not cached as a success");
});

const used = () => {
    std.gc();
    const u = memoryUsage();
    return u.mallocSize + u.memoryUsedSize;
};
__kitS.section("10k more distinct patterns do not grow memory", () => {
    for (let i = 0; i < 2000; i++) new RegExp("warm" + i + "[a-z]+\\d{2,5}(x|y)");
    const m1 = used();
    for (let i = 0; i < 10000; i++) new RegExp("adv" + i + "[a-z]+\\d{2,5}(x|y)");
    const m2 = used();
    for (let i = 0; i < 10000; i++) new RegExp("more" + i + "[a-z]+\\d{2,5}(x|y)");
    const m3 = used();
    ok(Math.abs(m3 - m2) < 64 * 1024, true, "10k more distinct patterns do not grow memory (" + (m2 - m1) + ", " + (m3 - m2) + " bytes)");
    for (let i = 0; i < 10000; i++) for (let k = 0; k < 2; k++) new RegExp("twice" + i + "[a-z]+\\d{2,5}(x|y)");
    const m4 = used();
    for (let i = 0; i < 10000; i++) for (let k = 0; k < 2; k++) new RegExp("again" + i + "[a-z]+\\d{2,5}(x|y)");
    const m5 = used();
    ok(Math.abs(m5 - m4) < 64 * 1024, true, "10k distinct patterns each compiled twice (admitted, then evicted) do not grow memory (" + (m4 - m3) + ", " + (m5 - m4) + " bytes)");
});
__kitS.section("a sliced pattern compiles to the right program", () => {
    const base = used();
    const compileSlice = () => {
        const big = "q".repeat(8 << 20) + "tail";
        const pat = big.slice(1000, 1100);
        new RegExp(pat);
        return new RegExp(pat).test("q".repeat(100));
    };
    ok(compileSlice(), true, "a sliced pattern compiles to the right program");
    const after = used();
    ok(after - base < 1 << 20, true, "a cached sliced pattern does not keep its 8 MB parent alive (" + (after - base) + " bytes)");
    ok(new RegExp("q".repeat(100)).test("q".repeat(100)), true, "the cached copy is found by an equal flat string");
});
__kitS.section("patterns over the size bound bypass the cache", () => {
    const big = "z".repeat(600);
    const re1 = new RegExp(big);
    const re2 = new RegExp(big, "i");
    ok([re1.test(big), re2.test(big.toUpperCase()), re1.test(big.toUpperCase())], [true, true, false], "patterns over the size bound bypass the cache");
});
__kitS.end();

print("test_regexp_compile_cache: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_regexp_compile_cache: " + fails + " failures");
