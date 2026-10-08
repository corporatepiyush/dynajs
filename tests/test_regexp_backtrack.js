let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

function assertThrows(ctor, fn, msg) {
    n++;
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof ctor)) {
            throw new Error("unexpected exception type: got " + e.name +
                            " (" + e.message + "), expected " + ctor.name +
                            (msg ? " [" + msg + "]" : ""));
        }
    }
    if (!threw)
        throw new Error("expected " + ctor.name + (msg ? " [" + msg + "]" : ""));
}

{
    const pat = "(a|" + Array.from({ length: 16 }, () => "a").join("|") + ")*b";
    const subject = "a".repeat(1024 * 1024);
    const t0 = Date.now();
    assertThrows(InternalError, () => {
        const re = new RegExp(pat);
        re.lastIndex = 0;
        return re.test(subject);
    }, "adversarial alternation must fail (not hang / not OOM)");
    const ms = Date.now() - t0;
    assert(ms < 2000, "adversarial match fails fast, not after the step budget (" + ms + " ms)");

}

{
    const pat = "(a|" + Array.from({ length: 16 }, () => "a").join("|") + ")*b";
    const t0 = Date.now();
    assertThrows(InternalError, () => {
        const re = new RegExp(pat);
        re.lastIndex = 0;
        return re.test("a".repeat(32 * 1024 * 1024));
    }, "adversarial alternation over a 32 MB subject fails catchably");
    assert(Date.now() - t0 < 2000, "32 MB subject fails fast too");
}

{
    const s = "a".repeat(1024 * 1024);
    assert(/^a*$/.test(s), "simple quantifier over 1 MB still matches");
    assert(/^.*$/.test(s), "greedy .* over 1 MB still matches");
    const lit = "z".repeat(1024 * 1024) + "needle";
    assert(/needle/.test(lit), "literal scan over 1 MB still matches");
    assert((s + "N").search(/N/) === s.length, "search over 1 MB finds its target");
}

{
    assert(/^(a|b)+$/.test("ababab"), "alternation class matches");
    assert(/^(a|b)+$/.test("abba"), "alternation class matches mixed");
    assert(!/^(a|b)+$/.test("abc"), "alternation class rejects invalid");

    const email = /^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$/;
    assert(email.test("user.name+tag@example.co.uk"), "email-ish matches");
    assert(!email.test("not-an-email"), "email-ish rejects invalid");

    assert(/^.*NEEDLE/.test("the quick brown fox NEEDLE"), "greedy .* ");
    assert(/^.*?NEEDLE/.test("the quick brown fox NEEDLE"), "lazy .*? ");

    const backref = /^(\w+)\s+\1$/;
    assert(backref.test("hello hello"), "backreference matches");
    assert(!backref.test("hello world"), "backreference rejects mismatch");

    assert(/^(?=.*\d)(?=.*[a-z])/.test("a1"), "positive lookahead");
    assert(!/^(?!.*\d)/.test("a1"), "negative lookahead rejects");

    assert(/^caf\u00e9$/.test("café"), "unicode literal matches");
    assert(/^[^\u0000-\u00ff]+$/.test("日本語"), "non-ASCII match");
    assert(/^\u{1F600}$/u.test("\u{1F600}"), "surrogate-pair unicode match");

    assert(/\d{4}-\d{2}-\d{2}/.test("2026-08-23"), "dated quantifier");
    assert(/(?:ab)+c/.test("ababc"), "non-capturing group repeat");
    assert(/x/.test("x"), "single char");
}

console.log("test_regexp_backtrack.js: " + n + " assertions passed");
