// covers: dyna:metrics.Metrics
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_metrics_metrics_upgrade");
import { Metrics } from "dyna:metrics";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor, msg + " (" + ctor.name + ", got " + caught + ")");
}

__kitS.section("st3_lat_bucket{route=\"a\",le=\"0.001\"} 0", () => {
    Metrics.reset();
    Metrics.histogram("st3_lat", 0.02, { route: "a" }, { buckets: [0.001, 0.05, 0.5] });
    Metrics.histogram("st3_lat", 3.0, { route: "a" });
    Metrics.histogram("st3_lat", 0.2, { route: "a" });
    let sc = Metrics.scrape();
    assert(sc.includes('st3_lat_bucket{route="a",le="0.001"} 0'),
        "custom edge 0.001 sits below the first observation");
    assert(sc.includes('st3_lat_bucket{route="a",le="0.05"} 1'),
        "custom edge 0.05 counted");
    assert(sc.includes('st3_lat_bucket{route="a",le="0.5"} 2'),
        "0.2 and 0.02 both land under the 0.5 band (cumulative)");
    assert(!sc.includes('le="1"'), "the default 1s edge is gone for this series");
    assert(sc.includes('le="+Inf"'), "the +Inf bucket remains");

    Metrics.histogram("st3_def", 0.02);
    sc = Metrics.scrape();
    assert(sc.includes('st3_def_bucket{le="0.005"}'), "default 5ms edge present");
    assert(sc.includes('st3_def_bucket{le="1"}'), "default 1s edge present");

    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [] }),
        RangeError, "an empty edge list is refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [1, 1] }),
        RangeError, "non-increasing edges refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [-1, 0.5] }),
        RangeError, "non-positive edges refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [Infinity] }),
        RangeError, "non-finite edges refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: "no" }),
        TypeError, "a non-array buckets is refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [0.1, "x"] }),
        RangeError, "a non-number edge is refused");
    throws(() => Metrics.histogram("st3_bad", 1, {},
        { buckets: [1, 2, 3, 4, 5, 6, 7] }), RangeError,
        "more than 6 edges refused");

    Metrics.histogram("st3_lbl", 0.02, { a: "1" }, { buckets: [0.5] });
    assert(Metrics.scrape().includes('st3_lbl_bucket{a="1",le="0.5"} 1'),
        "labels and custom edges compose");

    Metrics.reset();
});

__kitS.section("unknown option \"bucketz\" (valid: buckets)", () => {
    Metrics.reset();
    let caught = null;
    try { Metrics.histogram("cc6_lat", 0.02, {}, { bucketz: [0.5] }); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "bucketz" (valid: buckets)',
        "histogram opts name the key AND the valid set, got: " + caught);
    assert(!Metrics.scrape().includes("cc6_lat"),
        "the refused bag did not half-register the series");
    Metrics.histogram("cc6_lat", 0.02, {}, { buckets: [0.5] });
    assert(Metrics.scrape().includes("cc6_lat_bucket"), "the valid histogram bag is unchanged");
    throws(() => Metrics.histogram("cc6_lat2", 0.02, {}, 42), TypeError,
        "a primitive histogram bag is still refused outright");

    Metrics.reset();
});
__kitS.end();

print("test_metrics_metrics_upgrade: all tests passed (" + n + " assertions)");
