/* test_semver.js -- dyna:semver (in-repo SemVer 2.0.0 + npm-style ranges).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_semver.js
 * Prints "test_semver: all tests passed (N assertions)" on success; throws on
 * failure.
 *
 * Expected values are cross-checked several ways:
 *   1. hand vector tables with EXACT expected fields/results, drawn from the
 *      SemVer 2.0.0 spec (esp. the section-11 precedence example) and the
 *      documented npm `semver` range behavior (caret/tilde/hyphen/x-range/OR);
 *   2. an independent JS re-implementation of the section-11 precedence
 *      comparator (an algorithmic twin) diffed against the C module over tens
 *      of thousands of randomly generated versions (compare + sort);
 *   3. structural invariants (total-order consistency across the spec chain,
 *      round-trips through parse/clean/coerce). */

import * as sv from "dyna:semver";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let caught = false;
    try { fn(); } catch (e) { caught = true; }
    assert(caught, "should throw: " + msg);
}
function arrEq(a, b) {
    if (!Array.isArray(a) || !Array.isArray(b)) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
        if (Array.isArray(a[i]) || Array.isArray(b[i])) {
            if (!arrEq(a[i], b[i])) return false;
        } else if (a[i] !== b[i]) return false;
    }
    return true;
}
function sign(x) { return x < 0 ? -1 : x > 0 ? 1 : 0; }

/* deterministic PRNG (LCG) so any failure is reproducible */
let _seed = 0x2545F491 >>> 0;
function rnd() {
    _seed = (Math.imul(_seed, 1664525) + 1013904223) >>> 0;
    return _seed / 4294967296;
}
function ri(m) { return Math.floor(rnd() * m); }

/* ==================== parse: valid table (exact fields) ==================== */
{
    /* [input, major, minor, patch, prerelease[], build[], version] */
    const T = [
        ["0.0.0", 0, 0, 0, [], [], "0.0.0"],
        ["1.2.3", 1, 2, 3, [], [], "1.2.3"],
        ["10.20.30", 10, 20, 30, [], [], "10.20.30"],
        ["1.0.0-alpha", 1, 0, 0, ["alpha"], [], "1.0.0-alpha"],
        ["1.0.0-alpha.1", 1, 0, 0, ["alpha", 1], [], "1.0.0-alpha.1"],
        ["1.0.0-0.3.7", 1, 0, 0, [0, 3, 7], [], "1.0.0-0.3.7"],
        ["1.0.0-x.7.z.92", 1, 0, 0, ["x", 7, "z", 92], [], "1.0.0-x.7.z.92"],
        ["1.0.0-x-y-z.--", 1, 0, 0, ["x-y-z", "--"], [], "1.0.0-x-y-z.--"],
        ["1.2.3-beta.11", 1, 2, 3, ["beta", 11], [], "1.2.3-beta.11"],
        ["1.0.0+20130313144700", 1, 0, 0, [], ["20130313144700"], "1.0.0"],
        ["1.0.0-beta+exp.sha.5114f85", 1, 0, 0, ["beta"],
         ["exp", "sha", "5114f85"], "1.0.0-beta"],
        ["1.0.0+21AF26D3---117B344092BD", 1, 0, 0, [],
         ["21AF26D3---117B344092BD"], "1.0.0"],
        ["1.0.0-rc.1+build.123", 1, 0, 0, ["rc", 1], ["build", "123"],
         "1.0.0-rc.1"],
        /* build identifiers keep leading zeros and stay strings */
        ["1.2.3+001", 1, 2, 3, [], ["001"], "1.2.3"],
        ["1.2.3-0a", 1, 2, 3, ["0a"], [], "1.2.3-0a"],   /* alnum, not numeric */
        ["99999999999999.1.2", 99999999999999, 1, 2, [], [], "99999999999999.1.2"],
        ["0.0.0-0", 0, 0, 0, [0], [], "0.0.0-0"],
    ];
    for (const [inp, ma, mi, pa, pre, build, ver] of T) {
        const p = sv.parse(inp);
        assert(p.major === ma, "parse major " + inp + " => " + p.major);
        assert(p.minor === mi, "parse minor " + inp);
        assert(p.patch === pa, "parse patch " + inp);
        assert(arrEq(p.prerelease, pre),
            "parse prerelease " + inp + " => " + JSON.stringify(p.prerelease));
        assert(arrEq(p.build, build),
            "parse build " + inp + " => " + JSON.stringify(p.build));
        assert(p.version === ver, "parse version " + inp + " => " + p.version);
        assert(sv.isValid(inp) === true, "isValid true " + inp);
        /* field accessors agree */
        assert(sv.major(inp) === ma, "major() " + inp);
        assert(sv.minor(inp) === mi, "minor() " + inp);
        assert(sv.patch(inp) === pa, "patch() " + inp);
        assert(arrEq(sv.prerelease(inp), pre), "prerelease() " + inp);
        /* numeric prerelease identifiers are numbers, alnum are strings */
        for (const id of p.prerelease) {
            assert(typeof id === "number" || typeof id === "string",
                "prerelease id type " + inp);
        }
        for (const id of p.build)
            assert(typeof id === "string", "build id is string " + inp);
    }
}

/* ==================== parse: invalid table ==================== */
{
    const bad = [
        "", " ", "1", "1.2", "1.2.3.4", "1.2.x", "1.2.*",
        "v1.2.3",                    /* strict parse rejects the v prefix */
        "=1.2.3", "1.2.3 ", " 1.2.3",
        "01.2.3", "1.02.3", "1.2.03",     /* leading zeros in fields */
        "1.2.3-01",                       /* numeric prerelease leading zero */
        "1.2.3-beta.01",
        "1.2.3-", "1.2.3+", "1.2.3-+build",
        "1.2.3-beta..1", "1.2.3-.beta",
        "1.2.3-be!ta", "1.2.3+bu ild", "1.2.3_beta",
        "1.2.-3", "1.-2.3", "-1.2.3", "+1.2.3",
        "1.2.3.", ".1.2.3", "1..2.3",
        "a.b.c", "1.2.beta", "1.2.3beta",
        "1.2.3-beta+", "1.2.3++b",
        "0xff.0.0", "1e3.0.0",
        "99999999999999999999.0.0",       /* major > MAX_SAFE_INTEGER */
        "9007199254740992.0.0",           /* MAX_SAFE_INTEGER + 1 */
    ];
    for (const s of bad) {
        throws(() => sv.parse(s), "parse rejects " + JSON.stringify(s));
        assert(sv.isValid(s) === false, "isValid false " + JSON.stringify(s));
        throws(() => sv.major(s), "major() rejects " + JSON.stringify(s));
    }
    /* wrong argument types */
    throws(() => sv.parse(), "parse() no args");
    throws(() => sv.parse(123), "parse(number)");
    throws(() => sv.parse(null), "parse(null)");
    assert(sv.isValid(123) === false, "isValid(number) false");
    assert(sv.isValid(null) === false, "isValid(null) false");
    /* MAX_SAFE_INTEGER itself is valid */
    assert(sv.isValid("9007199254740991.0.0") === true, "MAX_SAFE valid");
}

/* ==================== clean ==================== */
{
    const T = [
        ["  =v1.2.3   ", "1.2.3"],
        ["v1.2.3", "1.2.3"],
        ["=1.2.3", "1.2.3"],
        ["vv1.2.3", "1.2.3"],
        ["  1.2.3  ", "1.2.3"],
        ["1.2.3", "1.2.3"],
        ["1.2.3-beta.1", "1.2.3-beta.1"],
        ["1.2.3+build", "1.2.3"],           /* build stripped from .version */
        ["=v1.2.3-rc.1+meta", "1.2.3-rc.1"],
        ["\t1.2.3\n", "1.2.3"],
    ];
    for (const [inp, exp] of T)
        assert(sv.clean(inp) === exp,
            "clean(" + JSON.stringify(inp) + ") => " + sv.clean(inp));
    /* unparseable => null */
    assert(sv.clean("not a version") === null, "clean garbage null");
    assert(sv.clean("1.2") === null, "clean partial null");
    assert(sv.clean("") === null, "clean empty null");
    assert(sv.clean("1.2.x") === null, "clean x-range null");
    throws(() => sv.clean(123), "clean(number) throws");
}

/* ==================== spec-11 precedence chain ==================== */
{
    /* SemVer 2.0.0 sec.11 example: strictly increasing precedence. */
    const chain = [
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
    ];
    for (let i = 0; i < chain.length; i++) {
        for (let j = 0; j < chain.length; j++) {
            const a = chain[i], b = chain[j];
            const exp = sign(i - j);
            assert(sv.compare(a, b) === exp,
                "compare(" + a + "," + b + ") == " + exp);
            assert(sv.gt(a, b) === (i > j), "gt " + a + " " + b);
            assert(sv.gte(a, b) === (i >= j), "gte " + a + " " + b);
            assert(sv.lt(a, b) === (i < j), "lt " + a + " " + b);
            assert(sv.lte(a, b) === (i <= j), "lte " + a + " " + b);
            assert(sv.eq(a, b) === (i === j), "eq " + a + " " + b);
            assert(sv.neq(a, b) === (i !== j), "neq " + a + " " + b);
        }
    }
    /* numeric identifiers compare numerically, not lexically */
    assert(sv.lt("1.0.0-beta.2", "1.0.0-beta.11"), "beta.2 < beta.11 numeric");
    assert(sv.lt("1.0.0-alpha.9", "1.0.0-alpha.10"), "alpha.9 < alpha.10");
    assert(sv.gt("1.0.0-alpha.10", "1.0.0-alpha.9"), "alpha.10 > alpha.9");
    /* numeric identifier < alphanumeric identifier */
    assert(sv.lt("1.0.0-1", "1.0.0-alpha"), "numeric < alnum");
    assert(sv.lt("1.0.0-alpha.1", "1.0.0-alpha.beta"), "alpha.1 < alpha.beta");
}

/* ==================== compare: build metadata ignored + misc ==================== */
{
    assert(sv.compare("1.0.0+build.1", "1.0.0+build.2") === 0,
        "build ignored in compare");
    assert(sv.eq("1.0.0+a", "1.0.0+b"), "build metadata equal");
    assert(sv.eq("1.0.0-beta+a", "1.0.0-beta+b"), "prerelease eq, build diff");
    assert(sv.compare("2.1.0", "2.0.9") === 1, "minor beats patch");
    assert(sv.compare("1.0.0", "0.9.9") === 1, "major beats minor");
    assert(sv.compare("1.0.0", "1.0.0") === 0, "identical");
    throws(() => sv.compare("1.0.0", "bad"), "compare invalid b");
    throws(() => sv.compare("bad", "1.0.0"), "compare invalid a");
    throws(() => sv.compare("1.0.0"), "compare missing b");
    throws(() => sv.gt("1.0.0"), "gt missing b");
}

/* ============= randomized differential compare vs a JS twin ============= */
{
    const ALPHA = ["alpha", "beta", "rc", "pre", "a", "b", "x", "dev",
                   "SNAPSHOT", "0a", "1z", "m", "final"];
    /* Build a random version: returns { str, struct } where struct.pre is an
     * array of numbers (numeric ids) and strings (alnum ids). */
    function genVersion() {
        const M = ri(4), m = ri(4), p = ri(4);
        const npre = ri(4);                 /* 0..3 prerelease identifiers */
        const pre = [];
        const parts = [];
        for (let i = 0; i < npre; i++) {
            if (rnd() < 0.5) {
                const num = ri(50);         /* numeric id, no leading zero */
                pre.push(num);
                parts.push(String(num));
            } else {
                const a = ALPHA[ri(ALPHA.length)];
                pre.push(a);
                parts.push(a);
            }
        }
        let str = M + "." + m + "." + p;
        if (parts.length) str += "-" + parts.join(".");
        return { str: str, struct: { major: M, minor: m, patch: p, pre: pre } };
    }
    /* Independent section-11 precedence comparator. */
    function twinCmp(a, b) {
        if (a.major !== b.major) return a.major < b.major ? -1 : 1;
        if (a.minor !== b.minor) return a.minor < b.minor ? -1 : 1;
        if (a.patch !== b.patch) return a.patch < b.patch ? -1 : 1;
        if (a.pre.length === 0 && b.pre.length === 0) return 0;
        if (a.pre.length === 0) return 1;
        if (b.pre.length === 0) return -1;
        const m = Math.min(a.pre.length, b.pre.length);
        for (let i = 0; i < m; i++) {
            const x = a.pre[i], y = b.pre[i];
            const xn = typeof x === "number", yn = typeof y === "number";
            let c;
            if (xn && yn) c = sign(x - y);
            else if (xn) c = -1;
            else if (yn) c = 1;
            else c = x < y ? -1 : x > y ? 1 : 0;
            if (c) return c;
        }
        return sign(a.pre.length - b.pre.length);
    }
    const N = 6000;
    for (let it = 0; it < N; it++) {
        const A = genVersion(), B = genVersion();
        const got = sv.compare(A.str, B.str);
        const exp = sign(twinCmp(A.struct, B.struct));
        assert(got === exp, "diff compare(" + A.str + "," + B.str + ") got " +
            got + " exp " + exp);
        /* antisymmetry */
        assert(sv.compare(B.str, A.str) === -exp, "antisymmetry " +
            A.str + " " + B.str);
        /* boolean helpers consistent with compare */
        assert(sv.gt(A.str, B.str) === (exp > 0), "gt consistent");
        assert(sv.eq(A.str, B.str) === (exp === 0), "eq consistent");
    }

    /* randomized sort: result is a permutation and non-decreasing */
    for (let it = 0; it < 200; it++) {
        const k = 2 + ri(20);
        const items = [];
        for (let i = 0; i < k; i++) items.push(genVersion());
        const input = items.map(x => x.str);
        const out = sv.sort(input.slice());
        assert(out.length === input.length, "sort length");
        /* permutation: multiset equal */
        const c1 = {}, c2 = {};
        for (const s of input) c1[s] = (c1[s] || 0) + 1;
        for (const s of out) c2[s] = (c2[s] || 0) + 1;
        for (const key in c1) assert(c1[key] === c2[key], "sort permutation");
        for (const key in c2) assert(c1[key] === c2[key], "sort permutation2");
        /* non-decreasing */
        for (let i = 1; i < out.length; i++)
            assert(sv.compare(out[i - 1], out[i]) <= 0, "sort ordered");
        /* input array not mutated */
        assert(input.length === k, "sort no mutate length");
    }
}

/* ==================== satisfies: big documented table ==================== */
{
    /* [version, range, expected] transcribed from the SemVer spec and the
     * documented npm range behavior (README expansions). */
    const T = [
        /* exact / comparators */
        ["1.2.3", "1.2.3", true],
        ["1.2.3", "=1.2.3", true],
        ["1.2.4", "1.2.3", false],
        ["1.2.3", ">1.2.2", true],
        ["1.2.3", ">1.2.3", false],
        ["1.2.3", ">=1.2.3", true],
        ["1.2.3", "<1.2.4", true],
        ["1.2.3", "<1.2.3", false],
        ["1.2.3", "<=1.2.3", true],
        ["1.2.3", ">=1.2.0 <1.3.0", true],
        ["1.3.0", ">=1.2.0 <1.3.0", false],
        ["1.2.3", ">1.0.0 <2.0.0", true],
        [" 1.2.3", "1.2.3", true],          /* npm trims the version too? no:
                                               this is our strict parse => the
                                               leading space is invalid input */
        /* caret */
        ["1.2.3", "^1.2.3", true],
        ["1.2.4", "^1.2.3", true],
        ["1.9.9", "^1.2.3", true],
        ["2.0.0", "^1.2.3", false],
        ["1.2.2", "^1.2.3", false],
        ["0.2.3", "^0.2.3", true],
        ["0.2.9", "^0.2.3", true],
        ["0.3.0", "^0.2.3", false],
        ["0.2.2", "^0.2.3", false],
        ["0.0.3", "^0.0.3", true],
        ["0.0.4", "^0.0.3", false],
        ["0.0.2", "^0.0.3", false],
        ["1.5.0", "^1.2.x", true],
        ["2.0.0", "^1.2.x", false],
        ["0.0.5", "^0.0.x", true],
        ["0.1.0", "^0.0.x", false],
        ["0.0.9", "^0.0", true],
        ["0.1.0", "^0.0", false],
        ["1.5.0", "^1.x", true],
        ["2.0.0", "^1.x", false],
        ["0.9.0", "^0.x", true],
        ["1.0.0", "^0.x", false],
        /* caret with prerelease (npm's documented allow/deny) */
        ["1.2.3-beta.2", "^1.2.3-beta.2", true],
        ["1.2.3-beta.4", "^1.2.3-beta.2", true],
        ["1.2.3", "^1.2.3-beta.2", true],
        ["1.9.0", "^1.2.3-beta.2", true],
        ["1.2.3-beta.1", "^1.2.3-beta.2", false],
        ["1.2.4-beta.2", "^1.2.3-beta.2", false],
        ["2.0.0", "^1.2.3-beta.2", false],
        ["0.0.3-beta", "^0.0.3-beta", true],
        ["0.0.3", "^0.0.3-beta", true],
        ["0.0.4", "^0.0.3-beta", false],
        /* tilde */
        ["1.2.3", "~1.2.3", true],
        ["1.2.9", "~1.2.3", true],
        ["1.3.0", "~1.2.3", false],
        ["1.2.2", "~1.2.3", false],
        ["1.2.0", "~1.2", true],
        ["1.2.9", "~1.2", true],
        ["1.3.0", "~1.2", false],
        ["1.0.0", "~1", true],
        ["1.9.9", "~1", true],
        ["2.0.0", "~1", false],
        ["0.2.3", "~0.2.3", true],
        ["0.2.9", "~0.2.3", true],
        ["0.3.0", "~0.2.3", false],
        ["0.2.0", "~0.2", true],
        ["0.3.0", "~0.2", false],
        ["0.5.0", "~0", true],
        ["1.0.0", "~0", false],
        ["1.2.3-beta.4", "~1.2.3-beta.2", true],
        ["1.2.4-beta.2", "~1.2.3-beta.2", false],
        ["1.2.9", "~1.2.3-beta.2", true],
        ["~>1.2.3", "~>1.2.3", false],       /* nonsense version arg: below */
        ["1.2.9", "~> 1.2.3", true],         /* ~> alias + spaced operator */
        /* x-ranges */
        ["1.5.0", "1.x", true],
        ["2.0.0", "1.x", false],
        ["1.2.5", "1.2.x", true],
        ["1.3.0", "1.2.x", false],
        ["1.5.0", "1", true],
        ["2.0.0", "1", false],
        ["1.2.5", "1.2", true],
        ["1.3.0", "1.2", false],
        ["3.1.4", "*", true],
        ["0.0.0", "*", true],
        ["3.1.4", "", true],                 /* empty range = any */
        ["1.2.3", "1.2.*", true],
        ["1.3.0", "1.2.*", false],
        ["9.9.9", "x", true],
        /* hyphen */
        ["1.2.3", "1.2.3 - 2.3.4", true],
        ["2.3.4", "1.2.3 - 2.3.4", true],
        ["1.5.0", "1.2.3 - 2.3.4", true],
        ["1.2.2", "1.2.3 - 2.3.4", false],
        ["2.3.5", "1.2.3 - 2.3.4", false],
        ["1.2.0", "1.2 - 2.3.4", true],
        ["2.3.4", "1.2 - 2.3.4", true],
        ["1.1.9", "1.2 - 2.3.4", false],
        ["2.3.9", "1.2.3 - 2.3", true],      /* <2.4.0 */
        ["2.4.0", "1.2.3 - 2.3", false],
        ["2.9.9", "1.2.3 - 2", true],        /* <3.0.0 */
        ["3.0.0", "1.2.3 - 2", false],
        /* OR sets */
        ["1.5.0", "^1.0.0 || ^2.0.0", true],
        ["2.5.0", "^1.0.0 || ^2.0.0", true],
        ["3.0.0", "^1.0.0 || ^2.0.0", false],
        ["1.0.0", "<1.0.0 || >=2.0.0", false],
        ["2.0.0", "<1.0.0 || >=2.0.0", true],
        ["0.9.0", "<1.0.0 || >=2.0.0", true],
        /* comparator with prerelease (the >1.2.3-alpha.3 example) */
        ["1.2.3-alpha.7", ">1.2.3-alpha.3", true],
        ["3.4.5-alpha.9", ">1.2.3-alpha.3", false],
        ["3.4.5", ">1.2.3-alpha.3", true],
        /* prerelease vs a non-prerelease range: excluded unless same tuple */
        ["1.2.3-alpha", "*", false],
        ["1.2.3-alpha", ">=1.0.0", false],
        ["1.2.3-alpha", "1.2.3-alpha", true],
        ["1.2.4-alpha", "^1.2.3", false],
        ["1.0.0-rc.1", "^1.0.0-rc.1", true],
        ["1.0.0-rc.2", "^1.0.0-rc.1", true],
        ["1.0.0", "^1.0.0-rc.1", true],
        ["1.0.1-rc.1", "^1.0.0-rc.1", false],
        /* combined comparators in one set (AND) */
        ["1.5.0", ">=1.2.0 <=1.9.0", true],
        ["1.9.1", ">=1.2.0 <=1.9.0", false],
        ["1.2.3", ">1.2.0 <1.2.3", false],
        /* spaced operators */
        ["1.5.0", ">= 1.0.0 < 2.0.0", true],
        ["2.5.0", ">= 1.0.0 < 2.0.0", false],
    ];
    for (const [ver, range, exp] of T) {
        /* the two "nonsense version" rows below use invalid version strings on
         * purpose to check that satisfies throws for a bad version */
        if (ver === "~>1.2.3" || ver === " 1.2.3") {
            throws(() => sv.satisfies(ver, range),
                "satisfies bad version " + JSON.stringify(ver));
            continue;
        }
        const got = sv.satisfies(ver, range);
        assert(got === exp, "satisfies(" + JSON.stringify(ver) + ", " +
            JSON.stringify(range) + ") == " + exp + " (got " + got + ")");
    }
    /* invalid range throws */
    throws(() => sv.satisfies("1.2.3", "not a range !!"), "bad range throws");
    throws(() => sv.satisfies("1.2.3", ">=x.y.z"), "bad range 2");
    throws(() => sv.satisfies("1.2.3", "==1.2.3"), "bad operator throws");
    throws(() => sv.satisfies("1.2.3"), "satisfies missing range");
}

/* ==================== inc: every release type ==================== */
{
    /* [version, release, identifier?, expected] */
    const T = [
        ["1.2.3", "major", undefined, "2.0.0"],
        ["1.2.3", "minor", undefined, "1.3.0"],
        ["1.2.3", "patch", undefined, "1.2.4"],
        ["1.2.0", "major", undefined, "2.0.0"],
        ["1.0.0", "minor", undefined, "1.1.0"],
        ["1.2.3", "premajor", undefined, "2.0.0-0"],
        ["1.2.3", "preminor", undefined, "1.3.0-0"],
        ["1.2.3", "prepatch", undefined, "1.2.4-0"],
        ["1.2.3", "prerelease", undefined, "1.2.4-0"],
        ["1.0.0", "prerelease", undefined, "1.0.1-0"],
        /* prerelease bumps of an existing prerelease */
        ["1.2.0-beta.1", "prerelease", undefined, "1.2.0-beta.2"],
        ["1.2.0-beta.0", "prerelease", undefined, "1.2.0-beta.1"],
        ["1.2.0-beta", "prerelease", undefined, "1.2.0-beta.0"],
        ["1.2.0-0", "prerelease", undefined, "1.2.0-1"],
        /* major/minor/patch off a prerelease "round down" to the release */
        ["1.0.0-5", "major", undefined, "1.0.0"],
        ["1.1.0-5", "major", undefined, "2.0.0"],
        ["1.2.0-5", "minor", undefined, "1.2.0"],
        ["1.2.1-5", "minor", undefined, "1.3.0"],
        ["1.2.0-5", "patch", undefined, "1.2.0"],
        ["1.2.3-5", "patch", undefined, "1.2.3"],
        /* with identifier */
        ["1.2.3", "premajor", "beta", "2.0.0-beta.0"],
        ["1.2.3", "preminor", "alpha", "1.3.0-alpha.0"],
        ["1.2.3", "prepatch", "rc", "1.2.4-rc.0"],
        ["1.2.3", "prerelease", "beta", "1.2.4-beta.0"],
        ["1.0.0", "prerelease", "beta", "1.0.1-beta.0"],
        ["1.2.0-beta.1", "prerelease", "beta", "1.2.0-beta.2"],
        ["1.2.0-beta.1", "prerelease", "alpha", "1.2.0-alpha.0"],
        ["1.2.0-alpha.0", "prerelease", "alpha", "1.2.0-alpha.1"],
        ["1.2.0-beta", "prerelease", "beta", "1.2.0-beta.0"],
    ];
    for (const [ver, rel, id, exp] of T) {
        const got = id === undefined ? sv.inc(ver, rel) : sv.inc(ver, rel, id);
        assert(got === exp, "inc(" + ver + ", " + rel +
            (id === undefined ? "" : ", " + id) + ") == " + exp +
            " (got " + got + ")");
        /* every inc result is itself a valid version */
        assert(sv.isValid(got), "inc result valid: " + got);
    }
    throws(() => sv.inc("1.2.3", "bogus"), "inc bad release");
    throws(() => sv.inc("bad", "major"), "inc bad version");
    throws(() => sv.inc("1.2.3"), "inc missing release");
    throws(() => sv.inc("1.2.3", "major", 5), "inc identifier must be string");
}

/* ==================== coerce ==================== */
{
    const T = [
        ["1.2.3", "1.2.3"],
        ["v1.2.3", "1.2.3"],
        ["=v1.2.3", "1.2.3"],
        ["v2.3.4-beta.1", "2.3.4"],
        ["1.2.3.4", "1.2.3"],
        ["42.6.7.9.3-alpha", "42.6.7"],
        ["v2", "2.0.0"],
        ["3.5", "3.5.0"],
        ["1", "1.0.0"],
        ["foo bar 3.4.5 baz", "3.4.5"],
        ["version 1.2.3 final", "1.2.3"],
        ["release-10.20.30-rc", "10.20.30"],
        ["  1.2.3  ", "1.2.3"],
        ["a.b.1.2", "1.2.0"],
    ];
    for (const [inp, exp] of T)
        assert(sv.coerce(inp) === exp,
            "coerce(" + JSON.stringify(inp) + ") == " + exp +
            " (got " + sv.coerce(inp) + ")");
    /* uncoercible => null */
    const nulls = ["", "version one", "no digits here", "...", "a.b.c"];
    for (const s of nulls)
        assert(sv.coerce(s) === null, "coerce null " + JSON.stringify(s));
    throws(() => sv.coerce(123), "coerce(number) throws");
    /* coerce output is always a valid version (when non-null) */
    for (const [inp] of T)
        assert(sv.isValid(sv.coerce(inp)), "coerce output valid " + inp);
}

/* ==================== sort: shuffled -> known order ==================== */
{
    const ordered = [
        "0.0.0", "0.0.1", "0.1.0", "1.0.0-alpha", "1.0.0-alpha.1",
        "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
        "1.0.0-rc.1", "1.0.0", "1.0.1", "1.2.0", "2.0.0", "10.0.0",
    ];
    /* deterministic shuffle */
    const shuffled = ordered.slice();
    for (let i = shuffled.length - 1; i > 0; i--) {
        const j = ri(i + 1);
        const t = shuffled[i]; shuffled[i] = shuffled[j]; shuffled[j] = t;
    }
    const out = sv.sort(shuffled);
    assert(arrEq(out, ordered), "sort produces the known order: " +
        JSON.stringify(out));
    /* new array, input not mutated to sorted */
    assert(out !== shuffled, "sort returns a new array");
    /* empty + singleton */
    assert(arrEq(sv.sort([]), []), "sort empty");
    assert(arrEq(sv.sort(["1.2.3"]), ["1.2.3"]), "sort singleton");
    /* stable for equal precedence (build metadata differs, same precedence) */
    const eqp = sv.sort(["1.0.0+b", "1.0.0+a", "1.0.0+c"]);
    assert(arrEq(eqp, ["1.0.0+b", "1.0.0+a", "1.0.0+c"]), "sort stable eqp");
    throws(() => sv.sort(["1.2.3", "bad"]), "sort rejects invalid element");
    throws(() => sv.sort(["1.2.3", 123]), "sort rejects non-string element");
    throws(() => sv.sort("nope"), "sort rejects non-array");
}

/* ==================== maxSatisfying / minSatisfying ==================== */
{
    const versions = ["1.2.0", "1.2.3", "1.3.0", "1.9.9", "2.0.0", "2.1.0",
                      "0.9.0"];
    assert(sv.maxSatisfying(versions, "^1.0.0") === "1.9.9", "maxSat ^1.0.0");
    assert(sv.minSatisfying(versions, "^1.0.0") === "1.2.0", "minSat ^1.0.0");
    assert(sv.maxSatisfying(versions, "~1.2.0") === "1.2.3", "maxSat ~1.2.0");
    assert(sv.minSatisfying(versions, "~1.2.0") === "1.2.0", "minSat ~1.2.0");
    assert(sv.maxSatisfying(versions, ">=2.0.0") === "2.1.0", "maxSat >=2.0.0");
    assert(sv.minSatisfying(versions, ">=2.0.0") === "2.0.0", "minSat >=2.0.0");
    assert(sv.maxSatisfying(versions, "^5.0.0") === null, "maxSat none null");
    assert(sv.minSatisfying(versions, "^5.0.0") === null, "minSat none null");
    assert(sv.maxSatisfying([], "*") === null, "maxSat empty null");
    assert(sv.maxSatisfying(versions, "*") === "2.1.0", "maxSat * is highest");
    assert(sv.minSatisfying(versions, "*") === "0.9.0", "minSat * is lowest");
    /* prerelease handling in maxSatisfying */
    const pre = ["1.0.0-alpha", "1.0.0-beta", "1.0.0"];
    assert(sv.maxSatisfying(pre, "^1.0.0-alpha") === "1.0.0", "maxSat pre");
    assert(sv.minSatisfying(pre, "^1.0.0-alpha") === "1.0.0-alpha", "minSat pre");
    throws(() => sv.maxSatisfying(["bad"], "*"), "maxSat rejects invalid");
    throws(() => sv.maxSatisfying(versions, "!!!"), "maxSat rejects bad range");
    throws(() => sv.maxSatisfying("nope", "*"), "maxSat rejects non-array");
}

/* ==================== field accessors on prerelease + edge ==================== */
{
    assert(sv.major("2.3.4") === 2, "major");
    assert(sv.minor("2.3.4") === 3, "minor");
    assert(sv.patch("2.3.4") === 4, "patch");
    assert(arrEq(sv.prerelease("1.2.3-beta.11"), ["beta", 11]), "prerelease");
    assert(arrEq(sv.prerelease("1.2.3"), []), "prerelease empty");
    assert(arrEq(sv.prerelease("1.2.3-0.a.9"), [0, "a", 9]), "prerelease mixed");
    throws(() => sv.prerelease("bad"), "prerelease rejects invalid");
}

print("test_semver: all tests passed (" + n + " assertions)");
