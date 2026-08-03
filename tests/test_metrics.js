/* Metrics: counters, gauges, histograms and the Prometheus text format.
   The registry is FIXED and the refusals are the point — a metric name or a
   label taken from a request would otherwise let a peer grow server memory
   without bound, so "the registry is full" must be an error, never a silent
   overwrite of somebody else's series. */
import { Metrics } from "dyna:net";

let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + ":\n  got  " + a + "\n  want " + b); } }
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { fails++; print("FAIL " + m + ": wrong reason -- " + e.message); } }
}
const line = (s, pre) => s.split("\n").find((l) => l.startsWith(pre));

/* --- a counter accumulates and never decreases --- */
Metrics.reset();
Metrics.counter("reqs_total", 1, { code: "200" });
Metrics.counter("reqs_total", 3, { code: "200" });
Metrics.counter("reqs_total", 5, { code: "500" });
{
    const s = Metrics.scrape();
    eq(line(s, 'reqs_total{code="200"}'), 'reqs_total{code="200"} 4', "counter accumulates");
    eq(line(s, 'reqs_total{code="500"}'), 'reqs_total{code="500"} 5',
       "a different label set is a different series");
    ok(s.indexOf("# TYPE reqs_total counter") >= 0, "the TYPE line is emitted");
}
threw(() => Metrics.counter("reqs_total", -1), /cannot decrease/, "a negative counter step");

/* --- a gauge is set, not added, and an unlabelled series emits no braces --- */
Metrics.reset();
Metrics.gauge("queue_depth", 42);
Metrics.gauge("queue_depth", 7);
eq(line(Metrics.scrape(), "queue_depth "), "queue_depth 7", "a gauge is the last value");
ok(Metrics.scrape().indexOf("queue_depth{}") < 0, "no empty braces without labels");

/* --- histogram buckets are CUMULATIVE, which is what the format means --- */
Metrics.reset();
for (const v of [0.003, 0.07, 2.5]) Metrics.histogram("lat", v);
{
    const s = Metrics.scrape();
    eq(line(s, 'lat_bucket{le="0.005"}'), 'lat_bucket{le="0.005"} 1', "le=0.005 counts one");
    eq(line(s, 'lat_bucket{le="0.05"}'), 'lat_bucket{le="0.05"} 1', "le=0.05 still one");
    eq(line(s, 'lat_bucket{le="0.1"}'), 'lat_bucket{le="0.1"} 2', "le=0.1 accumulates the second");
    eq(line(s, 'lat_bucket{le="+Inf"}'), 'lat_bucket{le="+Inf"} 3', "+Inf holds every observation");
    eq(line(s, "lat_count"), "lat_count 3", "count matches +Inf");
    ok(/^lat_sum 2\.57/.test(line(s, "lat_sum")), "sum is 2.573 (" + line(s, "lat_sum") + ")");
}

/* --- a label value that would break the grammar is escaped, not passed on.
   An unescaped quote makes the whole scrape body unparseable, so the
   collector drops every series, not just this one. --- */
Metrics.reset();
Metrics.counter("esc", 1, { path: 'a"b\\c' });
{
    const s = Metrics.scrape();
    ok(s.indexOf('path="a\\"b\\\\c"') >= 0, "quote and backslash escaped (" + line(s, "esc") + ")");
}

/* --- refusals --- */
Metrics.reset();
threw(() => Metrics.counter(), /name is required/, "no name");
threw(() => Metrics.counter("has spaces"), /not a valid/, "a name with a space");
threw(() => Metrics.counter("9leading"), /not a valid/, "a name starting with a digit");
threw(() => Metrics.counter("bad-dash"), /not a valid/, "a name with a dash");
threw(() => Metrics.gauge("g"), /value is required/, "a gauge with no value");
threw(() => Metrics.counter("x", 1, 42), /must be an object/, "non-object labels");

/* Registering one name as two types would emit two TYPE lines for one series,
   which the collector rejects. */
Metrics.reset();
Metrics.counter("dual", 1);
threw(() => Metrics.gauge("dual", 1), /already registered as another type/,
      "the same series as two types");

/* --- the bound is the defence: filling the registry REFUSES --- */
Metrics.reset();
{
    let refusedAt = -1;
    for (let i = 0; i < 400; i++) {
        try { Metrics.counter("m" + i, 1); }
        catch (e) { refusedAt = i; ok(/full/.test(e.message), "the refusal says the registry is full"); break; }
    }
    ok(refusedAt > 0 && refusedAt <= 256,
       "a fixed registry refuses rather than growing (refused at " + refusedAt + ")");
    /* and an already-registered series still works after the refusal */
    Metrics.counter("m0", 1);
    eq(line(Metrics.scrape(), "m0 "), "m0 2", "existing series keep working when full");
}

Metrics.reset();
eq(Metrics.scrape(), "", "reset empties the registry");

if (fails) {
    print("test_metrics: " + fails + " FAILED of " + n);
    throw new Error("test_metrics failed");
}
print("test_metrics: " + n + " assertions, 0 failures");
