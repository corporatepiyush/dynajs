// covers: c:src/core/crawlkit/crawlkit-robots.c
// Robots.allows picks the longest matching rule, Allow winning a tie, and
// allows when nothing matches. The rules are sorted once at parse time by
// (length desc, Allow first), so the first match decides, and indexed by the
// pattern's second byte: a rule whose first two bytes are literal can only
// match a path with the same second byte, so a lookup walks that bucket merged
// in sorted order with the rules that start with `*`, are shorter than two
// bytes, or are `X$`. At most 1000 rules are kept. Modperf round 2026-11-16: 2000 rules x 1000 paths
// 5.73 ms -> 2.09 ms. Oracle: a JS matcher over the same rules (wildcard `*`,
// end anchor `$`, longest match, Allow on ties), on paths and rules drawn from
// characters that the normaliser leaves unchanged.
import { suite } from "../lib/kit.js";
import { Robots } from "dyna:scrape";

const t = suite("test_scrape_robots_allows_bucket");

const RE = new Map();
function matchRule(pat, path) {
    let rx = RE.get(pat);
    if (!rx) {
        rx = compile(pat);
        RE.set(pat, rx);
    }
    return rx.test(path);
}
function compile(pat) {
    let re = "^";
    for (let i = 0; i < pat.length; i++) {
        const c = pat[i];
        if (c === "*") re += ".*";
        else if (c === "$" && i === pat.length - 1) re += "$";
        else re += c.replace(/[.\\^$|?+()[\]{}\/-]/g, "\\$&");
    }
    return new RegExp(re, "s");
}
function oracle(rules, path) {
    let best = -1, verdict = true;
    for (const [allow, pat] of rules) {
        if (!matchRule(pat, path)) continue;
        if (pat.length > best || (pat.length === best && allow)) {
            best = pat.length;
            verdict = allow;
        }
    }
    return verdict;
}

let seed = 0x5eed;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
const ALPHA = ["a", "b", "c", "/", ".", "-", "_", "0", "x"];
const piece = (k) => {
    let s = "";
    for (let i = 0; i < k; i++) s += ALPHA[rnd(ALPHA.length)];
    return s;
};
function randRule() {
    const shape = rnd(10);
    if (shape === 0) return "*" + piece(rnd(4));
    if (shape === 1) return "/";
    if (shape === 2) return "/*" + piece(rnd(3));
    if (shape === 3) return "/" + piece(rnd(2)) + "$";
    if (shape === 4) return piece(1 + rnd(3));
    if (shape === 5) return "/" + piece(1) + "*" + piece(rnd(3)) + (rnd(2) ? "$" : "");
    return "/" + piece(1 + rnd(5)) + (rnd(5) ? "" : "$");
}

t.test("random rule sets of every shape agree with the oracle", ({ eq }) => {
    for (let round = 0; round < 300; round++) {
        const rules = [];
        const nr = 1 + rnd(round < 100 ? 6 : 60);
        for (let i = 0; i < nr; i++) rules.push([rnd(2) === 0, randRule()]);
        const txt = "User-agent: *\n" + rules.map(([a, p]) => (a ? "Allow: " : "Disallow: ") + p).join("\n") + "\n";
        const r = new Robots(txt);
        let bad = 0, first = "";
        for (let j = 0; j < 40; j++) {
            const path = (rnd(10) ? "/" : "") + piece(rnd(7));
            const want = oracle(rules, path), got = r.allows(path);
            if (want !== got && !bad++) first = JSON.stringify(path) + " want " + want;
        }
        eq(bad, 0, "round " + round + " " + first + " rules " + JSON.stringify(rules));
        r.close();
    }
});

t.test("ties, empty paths and bucket edges", ({ eq }) => {
    const cases = [
        [[[false, "/ab"], [true, "/ab"]], ["/ab", "/abc", "/a", ""]],
        [[[true, "/ab"], [false, "/ab"]], ["/ab", "/abc"]],
        [[[false, "/a"], [true, "*a"]], ["/a", "/b", "a", ""]],
        [[[false, "/"], [true, "/$"]], ["/", "", "/a"]],
        [[[false, "/a$"], [true, "/a"]], ["/a", "/ab"]],
        [[[false, "ab"], [true, "a"]], ["ab", "abc", "/ab", "a"]],
        [[[false, "/b"], [true, "/*b"], [false, "/*"]], ["/b", "/xb", "/", "/bb"]],
        [[[false, "/xyz"], [true, "/x"], [false, "/xy"], [true, "/xyz"]], ["/xyz", "/xy", "/x", "/xyzz"]],
    ];
    for (const [rules, paths] of cases) {
        const txt = "User-agent: *\n" + rules.map(([a, p]) => (a ? "Allow: " : "Disallow: ") + p).join("\n") + "\n";
        const r = new Robots(txt);
        for (const p of paths) eq(r.allows(p), oracle(rules, p), JSON.stringify(rules) + " " + JSON.stringify(p));
        eq(r.ruleCount, rules.length, "ruleCount");
        r.close();
    }
});

t.test("a large rule set spread over many buckets", ({ eq }) => {
    const rules = [];
    for (let i = 0; i < 990; i++) rules.push([i % 3 === 0, "/" + "abcx0._-"[i % 8] + piece(2 + (i % 5)) + (i % 11 ? "" : "*" + piece(1))]);
    rules.push([false, "*.x$"], [true, "/"]);
    const txt = "User-agent: *\n" + rules.map(([a, p]) => (a ? "Allow: " : "Disallow: ") + p).join("\n") + "\n";
    const r = new Robots(txt);
    let bad = 0;
    for (let j = 0; j < 3000; j++) {
        const path = "/" + piece(1 + rnd(8)) + (rnd(4) ? "" : ".x");
        if (r.allows(path) !== oracle(rules, path)) bad++;
    }
    eq(bad, 0, "992 rules (the parser keeps at most 1000) x 3000 paths");
    r.close();
});

await t.run();
