// flags: --std
// tools/js/gen-dtoa-grisu.js -- regenerate the Grisu cached-power tables in src/dtoa.c: the
// GRISU_ALPHA_* defines, grisu_pow_hi / grisu_pow_lo (64-bit significand halves) and grisu_pow_ec
// (binary exponents) for every power of 10 in [-294, 342]. Exact BigInt arithmetic and a floor-log2
// search, not floating point, so the cached powers are correctly rounded; asserts the alpha=0
// sentinel (0x8000000000000000ULL, ec -127). Emits exactly the committed form (comment-free,
// clang-formatted) for tools/js/check-generated.js.
//
// Usage: ./dynajs --std tools/js/gen-dtoa-grisu.js   (stdout)
import * as std from "std";

const ALPHA_MIN = -294, ALPHA_MAX = 342;
const bitlen = (x) => (x === 0n ? 0 : x.toString(2).length);

function floorLog2(num, den) {
    let e2 = bitlen(num) - bitlen(den);
    const le = (e) => (e >= 0 ? (den << BigInt(e)) <= num : den <= num << BigInt(-e));
    while (!(le(e2) && !le(e2 + 1))) e2 += le(e2) ? 1 : -1;
    return e2;
}

function roundHalfEven(n, d) {
    const q = n / d, r = n - q * d, twice = 2n * r;
    if (twice > d || (twice === d && (q & 1n))) return [q + 1n, twice > d ? d - r : d - r];
    return [q, r];
}

function hex16(v) {
    return "0x" + v.toString(16).toUpperCase().padStart(16, "0") + "ULL";
}

function assert(c, m) {
    if (!c) throw new Error("AssertionError: " + m);
}

export function generate() {
    const hi = [], lo = [], ec = [];
    const M64 = (1n << 64n) - 1n;
    for (let alpha = ALPHA_MIN; alpha <= ALPHA_MAX; alpha++) {
        const num = alpha >= 0 ? 10n ** BigInt(alpha) : 1n;
        const den = alpha >= 0 ? 1n : 10n ** BigInt(-alpha);
        const e2 = floorLog2(num, den);
        const e = e2 - 127;
        const N = e <= 0 ? num << BigInt(-e) : num;
        const D = e <= 0 ? den : den << BigInt(e);
        const [f, rem] = roundHalfEven(N, D);
        assert((1n << 127n) <= f && f < 1n << 128n, alpha);
        assert(2n * rem <= D, alpha);
        hi.push(hex16(f >> 64n));
        lo.push(hex16(f & M64));
        ec.push(String(e));
    }
    const i0 = -ALPHA_MIN;
    assert(hi[i0] === "0x8000000000000000ULL" && ec[i0] === "-127", "sentinel");
    const out = [`#define GRISU_ALPHA_MIN (${ALPHA_MIN})`, `#define GRISU_ALPHA_MAX (${ALPHA_MAX})`,
        "#define GRISU_ALPHA_COUNT (GRISU_ALPHA_MAX - GRISU_ALPHA_MIN + 1)", ""];
    const arr = (decl, vals) => {
        out.push(decl);
        for (const v of vals) out.push("    " + v + ",");
        out.push("};");
    };
    arr("static const uint64_t grisu_pow_hi[GRISU_ALPHA_COUNT] = {", hi);
    out.push("");
    arr("static const uint64_t grisu_pow_lo[GRISU_ALPHA_COUNT] = {", lo);
    out.push("");
    arr("static const int32_t grisu_pow_ec[GRISU_ALPHA_COUNT] = {", ec);
    return out.join("\n") + "\n";
}

if (/gen-dtoa-grisu\.js$/.test(scriptArgs[0] || "")) std.out.puts(generate());
