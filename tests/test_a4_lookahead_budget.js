let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}

{
    const depth = 250;
    const body = "a,".repeat(500000) + "a";
    const src = "(".repeat(depth) + body + ")".repeat(depth);
    const t0 = Date.now();
    let ok = true;
    try {
        new Function(src);
    } catch (e) {
        ok = false;
        print("FAIL: nested-group source failed to compile: " + e.message);
    }
    const ms = Date.now() - t0;
    assert(ok, "250-deep group with a 1 MB body compiles");
    assert(ms < 2000, "lookahead work is bounded (took " + ms + " ms, cap 2000)");
}

{
    const src = "x = " + "(".repeat(100) + "1" + ")".repeat(100) + ";";
    const f = new Function(src + " return 1;");
    assert(typeof f === "function", "deep but small nesting still compiles");
}

print("test_a4_lookahead_budget: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_lookahead_budget: " + fails + " failures");
