// covers: dyna:semver.compare
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_compare");
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

let _seed = 0x2545F491 >>> 0;
function rnd() {
    _seed = (Math.imul(_seed, 1664525) + 1013904223) >>> 0;
    return _seed / 4294967296;
}
function ri(m) { return Math.floor(rnd() * m); }

__kitS.section("build ignored in compare", () => {
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
});

__kitS.section("diff compare", () => {
    const ALPHA = ["alpha", "beta", "rc", "pre", "a", "b", "x", "dev",
                   "SNAPSHOT", "0a", "1z", "m", "final"];
    function genVersion() {
        const M = ri(4), m = ri(4), p = ri(4);
        const npre = ri(4);
        const pre = [];
        const parts = [];
        for (let i = 0; i < npre; i++) {
            if (rnd() < 0.5) {
                const num = ri(50);
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
        assert(sv.compare(B.str, A.str) === -exp, "antisymmetry " +
            A.str + " " + B.str);
        assert(sv.gt(A.str, B.str) === (exp > 0), "gt consistent");
        assert(sv.eq(A.str, B.str) === (exp === 0), "eq consistent");
    }

    for (let it = 0; it < 200; it++) {
        const k = 2 + ri(20);
        const items = [];
        for (let i = 0; i < k; i++) items.push(genVersion());
        const input = items.map(x => x.str);
        const out = sv.sort(input.slice());
        assert(out.length === input.length, "sort length");
        const c1 = {}, c2 = {};
        for (const s of input) c1[s] = (c1[s] || 0) + 1;
        for (const s of out) c2[s] = (c2[s] || 0) + 1;
        for (const key in c1) assert(c1[key] === c2[key], "sort permutation");
        for (const key in c2) assert(c1[key] === c2[key], "sort permutation2");
        for (let i = 1; i < out.length; i++)
            assert(sv.compare(out[i - 1], out[i]) <= 0, "sort ordered");
        assert(input.length === k, "sort no mutate length");
    }
    const frozen = Object.freeze(["2.0.0", "1.0.0"]);
    const outF = sv.sort(frozen);
    assert(arrEq(outF, ["1.0.0", "2.0.0"]), "sort frozen array");
    assert(frozen[0] === "2.0.0", "frozen input untouched");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
