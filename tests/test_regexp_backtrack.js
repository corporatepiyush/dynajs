/* test_regexp_backtrack.js -- P1a-3: the regexp step budget bounds TIME but a
 * pattern like (a|a|a|...)*b against a long 'a' run holds the engine in a
 * split-state push that grows the backtrack stack WITHOUT bound: at the default
 * 1e8-step budget it can allocate GBs of heap before the budget fires, turning
 * a sub-second script into an OOM.
 *
 * The fix pairs the existing step budget (time) with a hard cap on the
 * backtrack stack (memory): growth past LRE_BACKTRACK_MAX_STACK_SIZE fails the
 * match with LRE_RET_MEMORY_ERROR, which surfaces in JS as a CATCHABLE
 * InternalError instead of the uncatchable "interrupted" step-budget abort.
 *
 * Run: ./dynajs tests/test_regexp_backtrack.js   (CONFIG_NATIVE_MODULES=y)
 */

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

/* ---- adversarial: the memory-ceiling case ----------------------------- */
/* 16 alternates, one 'a' each, then 'b'. Against an all-'a' subject every
 * split pushes a state, and the 1 MB subject makes that push unbounded. This
 * is exactly the audit's "reaches GBs" class. BEFORE the fix it grew the
 * backtrack stack to ~500 MB (measured peak RSS at 1 MB subject) and aborted
 * with the UNCATCHABLE InternalError:"interrupted" step-budget timeout. AFTER
 * the fix the stack cap fires first, failing fast and CATCHABLY. */
{
    const pat = "(a|" + Array.from({ length: 16 }, () => "a").join("|") + ")*b";
    const subject = "a".repeat(1024 * 1024); /* 1 MB */
    const t0 = Date.now();
    assertThrows(InternalError, () => {
        const re = new RegExp(pat);
        re.lastIndex = 0;
        return re.test(subject);
    }, "adversarial alternation must fail (not hang / not OOM)");
    const ms = Date.now() - t0;
    /* The cap fires on the stack growth, long before the 1e8-step budget, so
       the whole exec is a few tens of ms. A capped-but-step-bound implementation
       would take ~4 s; anything near that is a regression. */
    assert(ms < 2000, "adversarial match fails fast, not after the step budget (" + ms + " ms)");

    /* The failure must be a CATCHABLE exception (the script above already
       proved that: assertThrows returned, so the exception propagates normally
       rather than aborting the process the way the step-budget timeout does). */
}

/* The ceiling is input-size independent: a 32 MB subject fails in the same way
 * and at the same order of time as 1 MB, because the cap is a fixed byte bound
 * rather than a per-input growth. */
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

/* A big but LEGITIMATE match must still succeed -- the cap is only reached on
 * hostile inputs, not linear greedy scans. A 1 MB subject with a greedy dot
 * or an anchored class scan must match without tripping the cap. */
{
    const s = "a".repeat(1024 * 1024);
    assert(/^a*$/.test(s), "simple quantifier over 1 MB still matches");
    assert(/^.*$/.test(s), "greedy .* over 1 MB still matches");
    const lit = "z".repeat(1024 * 1024) + "needle";
    assert(/needle/.test(lit), "literal scan over 1 MB still matches");
    assert((s + "N").search(/N/) === s.length, "search over 1 MB finds its target");
}

/* ---- sanity: classic patterns still round-trip ------------------------- */
{
    assert(/^(a|b)+$/.test("ababab"), "alternation class matches");
    assert(/^(a|b)+$/.test("abba"), "alternation class matches mixed");
    assert(!/^(a|b)+$/.test("abc"), "alternation class rejects invalid");

    const email = /^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$/;
    assert(email.test("user.name+tag@example.co.uk"), "email-ish matches");
    assert(!email.test("not-an-email"), "email-ish rejects invalid");

    assert(/^.*NEEDLE/.test("the quick brown fox NEEDLE"), "greedy .* ");
    assert(/^.*?NEEDLE/.test("the quick brown fox NEEDLE"), "lazy .*? ");

    /* backreference */
    const backref = /^(\w+)\s+\1$/;
    assert(backref.test("hello hello"), "backreference matches");
    assert(!backref.test("hello world"), "backreference rejects mismatch");

    /* lookahead */
    assert(/^(?=.*\d)(?=.*[a-z])/.test("a1"), "positive lookahead");
    assert(!/^(?!.*\d)/.test("a1"), "negative lookahead rejects");

    /* unicode */
    assert(/^caf\u00e9$/.test("café"), "unicode literal matches");
    assert(/^[^\u0000-\u00ff]+$/.test("日本語"), "non-ASCII match");
    assert(/^\u{1F600}$/u.test("\u{1F600}"), "surrogate-pair unicode match");

    /* quantifier shapes from the suite */
    assert(/\d{4}-\d{2}-\d{2}/.test("2026-08-23"), "dated quantifier");
    assert(/(?:ab)+c/.test("ababc"), "non-capturing group repeat");
    assert(/x/.test("x"), "single char");
}

console.log("test_regexp_backtrack.js: " + n + " assertions passed");
