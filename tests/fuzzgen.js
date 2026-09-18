/* fuzzgen.js -- ADVERSARIAL DATA GENERATION. Nothing else.
 *
 * This module generates values. It does not assert, it does not time, it does
 * not print, and it never decides whether anything passed. That separation is
 * deliberate: the moment a generator also judges, every suite that imports it
 * inherits that judgement, and a corpus can no longer be shared between a
 * pen test (which asserts refusal), a parametric test (which asserts a value)
 * and a benchmark (which asserts nothing and must not be perturbed).
 *
 * Consumers:
 *   tests/test_api_fuzz.js       randomised sweep
 *   tests/test_api_params.js     hand-written value tables
 *   tests/test_api_surface.js    fixed matrix over every name
 *   tests/test_*_pentest.js      adversarial suites
 *
 * THE CORPUS IS VENDORED, NOT INVENTED: tests/corpus/blns.txt is the Big List
 * of Naughty Strings, 516 entries across ~36 categories that real projects
 * maintain. A hand-written list contains the strings its author thought of;
 * this one contains the strings that have actually broken software.
 */
import * as std from "std";

/* ------------------------------------------------------------ the corpus */

const LONE_HI = String.fromCharCode(0xd800);
const LONE_LO = String.fromCharCode(0xdfff);

/* The floor: kept so a missing corpus file degrades rather than empties. */
const FLOOR_STRINGS = [
    "", " ", "\0", "\n", "\r\n", "\t", "\\", "\"", "'", "`",
    LONE_HI, LONE_LO, LONE_HI + LONE_LO, "\u{1f600}", "é",
    "../", "..\\", "%2e%2e", "%00", "javascript:", "<script>",
    "__proto__", "constructor", "prototype", "toString",
    "-1", "0x10", "1e999", "NaN", "Infinity", "null", "undefined",
    "0".repeat(400), "a".repeat(4096), "{", "[", "}", "]",
];

let corpusCount = 0;

function loadCorpus() {
    const out = FLOOR_STRINGS.slice();
    try {
        const body = std.loadFile("tests/corpus/blns.txt");
        if (body) {
            for (const line of body.split("\n")) {
                if (line === "" || line.charCodeAt(0) === 35 /* # */) continue;
                out.push(line);
                corpusCount++;
            }
        }
    } catch (e) { /* floor only */ }
    return out;
}

/** Every adversarial string: the vendored corpus plus the floor list. */
export const STRINGS = loadCorpus();

/** How many came from the vendored file. Zero means the caller is running on
 *  the floor list and should SAY so -- silent thin coverage reads as green. */
export const corpusSize = () => corpusCount;

/* Numbers on the boundaries an implementation branches at. A uniform double
   essentially never lands on 2^31, which is why random alone finds nothing. */
export const NUMBERS = [
    0, -0, 1, -1, 2, -2, 0.5, -0.5, NaN, Infinity, -Infinity,
    127, 128, 255, 256, 32767, 32768, 65535, 65536,
    2147483647, -2147483648, 2147483648, 4294967295, 4294967296,
    9007199254740991, -9007199254740991, 9007199254740992,
    1e-300, 1e300, Number.MIN_VALUE, Number.MAX_VALUE,
];

/** Header values a client fully controls: framing, injection, blowup. */
export const HEADERS = [
    "", " ", "\0", "\r\n", "a\r\nInjected: 1", "=", ";;;;;;;;;;",
    "a=b; ".repeat(20000), "a".repeat(65536), "a=" + "b".repeat(65536),
    '"' + "a".repeat(65536), "\\".repeat(65536),
    "text/plain; " + "p=1; ".repeat(20000),
    "bytes=0-" + "9".repeat(400), "bytes=-1", "bytes=" + "0-1,".repeat(20000),
    "bytes=9223372036854775807-9223372036854775807", "*/*;q=" + "9".repeat(400),
];

/** Path shapes: traversal in every spelling, plus the Windows device names. */
export const PATHS = [
    "", ".", "..", "/", "//", "/".repeat(4000),
    "../".repeat(4000) + "etc/passwd", "..\\..\\etc\\passwd",
    "%2e%2e/etc/passwd", "%252e%252e/etc/passwd", "..%5c",
    "/a\0b", "a\0.png", "/" + "a".repeat(65536), LONE_HI,
    "CON", "PRN", "AUX", "NUL", "COM1", "LPT1",
];

/* -------------------------------------------------------------- the PRNG */

/* xorshift128+, written out rather than imported: a generator must not depend
   on the module family it is used to test. */
export function PRNG(seed) {
    let s0 = seed >>> 0 || 0x9e3779b9;
    let s1 = (seed * 2654435761) >>> 0 || 0x85ebca6b;
    return function () {
        let x = s0;
        const y = s1;
        s0 = y;
        x ^= x << 23; x >>>= 0;
        x ^= x >>> 17;
        x ^= y ^ (y >>> 26);
        s1 = x >>> 0;
        return ((s0 + s1) >>> 0) / 4294967296;
    };
}

/* ---------------------------------------------------------- value shapes */

const VIEWS = [Uint8Array, Int8Array, Uint16Array, Int32Array,
               Float32Array, Float64Array];

/** Build a generator bound to one PRNG. Everything below is pure data. */
export function generator(rnd) {
    const pick = (a) => a[Math.floor(rnd() * a.length) % a.length];

    function gen(depth) {
        const r = rnd();
        if (depth > 3) return r < 0.5 ? pick(NUMBERS) : pick(STRINGS);
        if (r < 0.20) return pick(NUMBERS);
        if (r < 0.42) return pick(STRINGS);
        if (r < 0.48) return rnd() < 0.5;
        if (r < 0.52) return null;
        if (r < 0.56) return undefined;
        if (r < 0.62) {
            const K = pick(VIEWS), n = Math.floor(rnd() * 9), a = new K(n);
            for (let i = 0; i < n; i++) a[i] = pick(NUMBERS);
            return a;
        }
        if (r < 0.70) {
            const n = Math.floor(rnd() * 5), a = [];
            for (let i = 0; i < n; i++) a.push(gen(depth + 1));
            return a;
        }
        if (r < 0.78) {
            const n = Math.floor(rnd() * 4), o = {};
            for (let i = 0; i < n; i++) o[pick(STRINGS)] = gen(depth + 1);
            return o;
        }
        if (r < 0.82) {          /* recursive: a value containing its own kind */
            const n = Math.floor(rnd() * 3) + 1, a = [];
            for (let i = 0; i < n; i++) a.push(gen(depth + 1));
            return rnd() < 0.5 ? a : { nested: a, self: gen(depth + 1) };
        }
        if (r < 0.86) return () => gen(depth + 1);
        if (r < 0.90) return { valueOf() { throw new Error("valueOf"); } };
        if (r < 0.94) return { toString() { throw new Error("toString"); } };
        if (r < 0.97) return new Proxy({}, { get() { throw new Error("trap"); } });
        return Symbol("s");
    }

    return {
        pick,
        value: () => gen(0),
        /** A tuple of 0..max arguments. */
        args(max) {
            const n = Math.floor(rnd() * ((max || 3) + 1));
            const a = [];
            for (let i = 0; i < n; i++) a.push(gen(0));
            return a;
        },
        /** Exactly `n` bytes of noise. */
        bytes(n) {
            const a = new Uint8Array(n);
            for (let i = 0; i < n; i++) a[i] = Math.floor(rnd() * 256);
            return a;
        },
    };
}

/* ------------------------------------------------------------- shrinking */

/* Simpler candidates for one value. Ordered cheapest-first: the empty/zero
   case is both the biggest simplification and the likeliest to still fail. */
export function candidates(v) {
    const out = [];
    if (typeof v === "string" && v.length) {
        out.push("", v.slice(0, v.length >> 1), v.slice(0, 1));
        if (v.length > 1) out.push(v.slice(-1));
    } else if (typeof v === "number" && v !== 0) {
        out.push(0);
        if (Number.isFinite(v)) out.push(Math.trunc(v / 2), Math.sign(v));
    } else if (Array.isArray(v) && v.length) {
        out.push([], v.slice(0, v.length >> 1));
        for (let i = 0; i < v.length && i < 4; i++)
            out.push(v.slice(0, i).concat(v.slice(i + 1)));
    } else if (ArrayBuffer.isView(v) && v.length) {
        out.push(new v.constructor(0), v.slice(0, v.length >> 1));
    } else if (v && typeof v === "object") {
        const k = Object.keys(v);
        if (k.length) {
            out.push({});
            for (let i = 0; i < k.length && i < 4; i++) {
                const c = {};
                for (const kk of k) if (kk !== k[i]) c[kk] = v[kk];
                out.push(c);
            }
        }
    }
    return out;
}

/**
 * The smallest tuple still exhibiting the failure.
 *
 * `still(tuple)` MUST test the same symptom that failed. If it tests something
 * else, shrinking walks toward a different bug than the one found -- and the
 * minimal case it reports will be for that other bug.
 *
 * Bounded on purpose: shrinking that runs longer than the search is its own
 * denial of service.
 */
export function shrink(args, still, budget) {
    let best = args.slice();
    let steps = budget || 120;
    let moved = true;
    while (moved && steps > 0) {
        moved = false;
        for (let i = 0; i < best.length && steps > 0; i++) {   /* drop an arg */
            const t = best.slice(0, i).concat(best.slice(i + 1));
            steps--;
            if (still(t)) { best = t; moved = true; break; }
        }
        if (moved) continue;
        for (let i = 0; i < best.length && steps > 0; i++) {   /* simplify one */
            for (const c of candidates(best[i])) {
                if (steps-- <= 0) break;
                const t = best.slice();
                t[i] = c;
                if (still(t)) { best = t; moved = true; break; }
            }
            if (moved) break;
        }
    }
    return best;
}

/** Printable form of a tuple. A failure nobody can read is not a report. */
export function show(a) {
    return "[" + a.map((v) => {
        try {
            if (typeof v === "function") return "fn";
            if (typeof v === "symbol") return "symbol";
            if (ArrayBuffer.isView(v)) return v.constructor.name + "(" + v.length + ")";
            const s = JSON.stringify(v);
            return s === undefined ? String(v)
                 : (s.length > 40 ? s.slice(0, 40) + "…" : s);
        } catch (e) { return "<throws>"; }
    }).join(", ") + "]";
}
