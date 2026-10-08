// flags: --std
import { Metrics } from "dyna:net";
import * as std from "std";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    n++;
    if (!(caught instanceof ctor))
        throw new Error(msg + " (expected " + ctor.name + ", got " + caught + ")");
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error(msg + " (got " + JSON.stringify(got) +
                        ", want " + JSON.stringify(want) + ")");
}
function gcPressure() {
    let junk = [];
    for (let i = 0; i < 32; i++)
        junk.push("x".repeat(16 + (i % 8) * 24) + String(i) + { a: i }.a);
    junk = null;
    if (std && typeof std.gc === "function") std.gc();
}
function bucketLine(sc, name, labels, le) {
    const want = name + "_bucket{" + (labels ? labels + "," : "") + 'le="' + le + '"';
    for (const ln of sc.split("\n"))
        if (ln.startsWith(want)) return ln;
    return null;
}

{
    Metrics.reset();
    throws(() => Metrics.histogram("m0", 1, {}, { buckets: [] }),
        RangeError, "0 edges (empty array) refused");
    assert(!Metrics.scrape().includes("m0_bucket"), "the 0-edge refusal registered nothing");

    Metrics.histogram("m1", 0.5, {}, { buckets: [1] });
    Metrics.histogram("m1", 1.5, {}, { buckets: [1] });
    Metrics.histogram("m1", 1, {}, { buckets: [1] });
    let sc = Metrics.scrape();
    eq(bucketLine(sc, "m1", "", "1"), 'm1_bucket{le="1"} 2',
        "1-edge series: values <= 1 counted twice (0.5 and the exact edge)");
    assert(sc.includes('m1_bucket{le="+Inf"} 3'), "1-edge series: +Inf counts all three");
    assert(sc.includes("m1_count 3"), "1-edge series: count is 3");
    assert(sc.includes("m1_sum 3"), "1-edge series: sum is 0.5+1.5+1 = 3");

    const six = [0.001, 0.01, 0.1, 1, 10, 100];
    Metrics.histogram("m6", 50, {}, { buckets: six });
    Metrics.histogram("m6", 0.0005, {}, { buckets: six });
    sc = Metrics.scrape();
    eq(bucketLine(sc, "m6", "", "0.001"), 'm6_bucket{le="0.001"} 1',
        "6-edge series: only the sub-millisecond sample lands on edge 1");
    eq(bucketLine(sc, "m6", "", "0.01"), 'm6_bucket{le="0.01"} 1',
        "6-edge series: cumulative count carries upward");
    eq(bucketLine(sc, "m6", "", "10"), 'm6_bucket{le="10"} 1',
        "6-edge series: 50 is above 10");
    eq(bucketLine(sc, "m6", "", "100"), 'm6_bucket{le="100"} 2',
        "6-edge series: both samples land under the top edge");
    assert(!bucketLine(sc, "m6", "", "1000"),
        "6-edge series: no phantom edge beyond the array");

    throws(() => Metrics.histogram("m7", 1, {}, { buckets: [1, 2, 3, 4, 5, 6, 7] }),
        RangeError, "7 edges (MET_NBUCKET+1) refused");
    assert(!Metrics.scrape().includes("m7_bucket"), "the 7-edge refusal registered nothing");

    let caught = null;
    try { Metrics.histogram("m7b", 1, {}, { buckets: [1, 2, 3, 4, 5, 6, 7] }); }
    catch (e) { caught = e; }
    n++;
    if (!/1 to 6/.test(String(caught && caught.message)))
        throw new Error("the refusal states the real bound 1 to 6 (got: " + caught + ")");
    Metrics.reset();
}

{
    Metrics.reset();
    for (let round = 0; round < 50; round++) {
        Metrics.histogram("rep_a", 0.45, { s: "a" }, { buckets: [0.5] });
        Metrics.histogram("rep_a", 0.55, { s: "a" },
            { buckets: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6] });
        Metrics.histogram("rep_b", 1.5, { s: "b" }, { buckets: [1, 2, 3] });
        Metrics.histogram("rep_b", 150, { s: "b" }, { buckets: [100] });
        if (round % 10 === 0) gcPressure();
    }
    let sc = Metrics.scrape();
    eq(bucketLine(sc, "rep_a", 's="a"', "0.5"), 'rep_a_bucket{s="a",le="0.5"} 50',
        "rep_a keeps the FIRST array across 100 observes");
    assert(sc.includes('rep_a_bucket{s="a",le="+Inf"} 100'),
        "rep_a: all 100 samples counted once");
    eq(bucketLine(sc, "rep_b", 's="b"', "1"), 'rep_b_bucket{s="b",le="1"} 0',
        "rep_b edge 1 has none of the 1.5 samples");
    eq(bucketLine(sc, "rep_b", 's="b"', "3"), 'rep_b_bucket{s="b",le="3"} 50',
        "rep_b edge 3 has the 50 in-band samples (the 150s are above)");
    assert(!sc.includes('le="100"'), "the second (ignored) arrays never appear");
    Metrics.reset();
}

{
    Metrics.reset();
    for (let i = 0; i < 200; i++) {
        Metrics.histogram("int_wide", i / 100, { k: String(i % 3) },
            { buckets: [0.5, 1, 1.5, 2, 2.5, 3] });
        Metrics.histogram("int_narrow", i / 100, { k: String(i % 3) }, { buckets: [2.75] });
        Metrics.histogram("int_mid", i / 100, { k: String(i % 3) }, { buckets: [1, 2] });
        if (i % 25 === 0) gcPressure();
    }
    const sc = Metrics.scrape();
    for (const k of ["0", "1", "2"]) {
        const lbl = 'k="' + k + '"';
        assert(bucketLine(sc, "int_wide", lbl, "3"),
            "int_wide keeps its own 6th edge for k=" + k);
        assert(bucketLine(sc, "int_narrow", lbl, "2.75"),
            "int_narrow keeps its single edge for k=" + k);
        assert(bucketLine(sc, "int_mid", lbl, "2"),
            "int_mid keeps its 2 edges for k=" + k);
        assert(!bucketLine(sc, "int_mid", lbl, "2.5"),
            "int_mid did not absorb int_wide's edges for k=" + k);
        assert(!bucketLine(sc, "int_narrow", lbl, "0.5"),
            "int_narrow did not absorb int_wide's edges for k=" + k);
    }
    const wide1 = bucketLine(sc, "int_wide", 'k="1"', "3");
    const cnt = parseInt(wide1.slice(wide1.lastIndexOf(" ") + 1), 10);
    assert(cnt >= 66 && cnt <= 68, "int_wide k=1 cumulative count is sane: " + cnt);
    Metrics.reset();
}

{
    Metrics.reset();
    for (let i = 0; i < 300; i++) {
        const bag = { buckets: [0.01, 0.02, 0.03, 0.04, 0.05, 0.06] };
        if (i % 3 === 0) bag.other = "y".repeat(64);
        try { Metrics.histogram("gc_h", i / 1000, { i: String(i % 2) }, bag); }
        catch (e) {
            assert(e instanceof TypeError && /unknown option/.test(String(e.message)),
                "strict bag still refuses unknown keys under churn (got: " + e + ")");
        }
        gcPressure();
    }
    const sc = Metrics.scrape();
    for (const k of ["0", "1"]) {
        assert(bucketLine(sc, "gc_h", 'i="' + k + '"', "0.06"),
            "gc-churned series keeps all 6 edges for i=" + k);
    }
    let total = 0;
    for (const k of ["0", "1"]) {
        const ln = bucketLine(sc, "gc_h", 'i="' + k + '"', "+Inf");
        total += parseInt(ln.slice(ln.lastIndexOf(" ") + 1), 10);
    }
    eq(total, 200, "exactly the non-refused observes counted");
    Metrics.reset();
}

{
    Metrics.reset();
    Metrics.histogram("val", 2, {}, { buckets: [1, 2, 3] });
    Metrics.histogram("val", 2 - Number.EPSILON, {}, { buckets: [1, 2, 3] });
    Metrics.histogram("val", 0, {}, { buckets: [1, 2, 3] });
    Metrics.histogram("val", 1e-300, {}, { buckets: [1, 2, 3] });
    Metrics.histogram("val", 1e300, {}, { buckets: [1, 2, 3] });
    let sc = Metrics.scrape();
    eq(bucketLine(sc, "val", "", "1"), 'val_bucket{le="1"} 2',
        "0 and 1e-300 land under edge 1");
    eq(bucketLine(sc, "val", "", "2"), 'val_bucket{le="2"} 4',
        "the exact edge 2 and the epsilon below both count under 2");
    eq(bucketLine(sc, "val", "", "3"), 'val_bucket{le="3"} 4',
        "nothing between 2 and 3");
    assert(sc.includes("val_count 5") && sc.includes("val_sum 1.0000000000000001e+300"),
        "extreme values keep count and sum straight");

    throws(() => Metrics.histogram("val", NaN, {}, { buckets: [1] }),
        RangeError, "NaN refused");
    throws(() => Metrics.histogram("val", Infinity, {}, { buckets: [1] }),
        RangeError, "Infinity refused");
    Metrics.reset();
}

{
    Metrics.reset();
    const bad = [
        () => Metrics.histogram("ref", 1, {}, { buckets: [] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [1, 2, 3, 4, 5, 6, 7] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [2, 1] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [1, 1] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [0] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [-0.5] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [NaN] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: [Infinity] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: ["1"] }),
        () => Metrics.histogram("ref", 1, {}, { buckets: {} }),
        () => Metrics.histogram("ref", 1, {}, { buckets: "0.5" }),
    ];
    for (let round = 0; round < 200; round++) {
        for (const f of bad) {
            let caught = null;
            try { f(); } catch (e) { caught = e; }
            n++;
            if (caught === null)
                throw new Error("a malformed buckets array was accepted at round " + round);
        }
        if (round % 40 === 0) gcPressure();
    }
    assert(!Metrics.scrape().includes("ref_"),
        "2200 refusals registered nothing (the refusal path leaves no half-series)");
    Metrics.histogram("ref_ok", 0.5, {}, { buckets: [1] });
    assert(Metrics.scrape().includes('ref_ok_bucket{le="1"} 1'),
        "a legal call after the refusal churn works");
    Metrics.reset();
}

{
    Metrics.reset();
    Metrics.histogram("mix", 0.5, { route: "/a", code: "200" }, { buckets: [1, 2] });
    Metrics.histogram("mix", 1.5, { route: "/a", code: "200" }, { buckets: [1, 2] });
    Metrics.histogram("mix", 1.5, { route: "/b", code: "500" }, { buckets: [1, 2] });
    let caught = null;
    try { Metrics.counter("mix", 3, { route: "/a", code: "200" }); } catch (e) { caught = e; }
    assert(caught instanceof TypeError && /another type/.test(String(caught.message)),
        "a histogram series cannot be re-registered as a counter (same name+labels)");
    const sc = Metrics.scrape();
    assert(sc.includes('mix_bucket{route="/a",code="200",le="1"} 1'),
        "labels render with custom edges");
    assert(sc.includes('mix_bucket{route="/a",code="200",le="2"} 2'),
        "cumulative buckets under labels");
    assert(sc.includes('mix_bucket{route="/b",code="500",le="+Inf"} 1'),
        "second label set carries its own series");
    assert(!sc.includes('mix_bucket{route="/a",code="200",le="3"'),
        "no edge above the registered array");
    Metrics.reset();
}

{
    Metrics.reset();
    Metrics.histogram("first", 0.5, {}, { buckets: [7] });
    for (let i = 0; i < 25; i++)
        Metrics.histogram("first", 6.5, {}, { buckets: [1, 2, 3, 4, 5, 6] });
    const sc = Metrics.scrape();
    eq(bucketLine(sc, "first", "", "7"), 'first_bucket{le="7"} 26',
        "all 26 samples sit under the FIRST registration's edge");
    assert(!bucketLine(sc, "first", "", "6"),
        "the later arrays never became edges");
    Metrics.histogram("defaults", 0.07);
    const sc2 = Metrics.scrape();
    assert(sc2.includes('defaults_bucket{le="0.05"} 0') &&
           sc2.includes('defaults_bucket{le="0.1"} 1') &&
           sc2.includes('defaults_bucket{le="1"} 1'),
        "default edges intact next to custom series");
    Metrics.reset();
}

print("test_metrics_custom_buckets: all tests passed (" + n + " assertions)");
