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
threw(() => Metrics.counter("reqs_total", -1), /increment must be/, "a negative counter step");

Metrics.reset();
Metrics.gauge("queue_depth", 42);
Metrics.gauge("queue_depth", 7);
eq(line(Metrics.scrape(), "queue_depth "), "queue_depth 7", "a gauge is the last value");
ok(Metrics.scrape().indexOf("queue_depth{}") < 0, "no empty braces without labels");

Metrics.reset();
Metrics.gauge("g_nan", NaN);
Metrics.gauge("g_pinf", Infinity);
Metrics.gauge("g_ninf", -Infinity);
{
    const s = Metrics.scrape();
    eq(line(s, "g_nan "), "g_nan NaN", "NaN emits the NaN token");
    eq(line(s, "g_pinf "), "g_pinf +Inf", "+Inf emits the +Inf token");
    eq(line(s, "g_ninf "), "g_ninf -Inf", "-Inf emits the -Inf token");
    ok(s.split("\n").filter((l) => l && !l.startsWith("#")).every((l) => {
        const v = l.slice(l.lastIndexOf(" ") + 1);
        return !/^[+-]?inf$/.test(v) && !/^[+-]?nan$/.test(v);
    }), "no bare inf/nan spelling leaks");
}
Metrics.reset();
Metrics.gauge("g_ok", 1.5);
eq(line(Metrics.scrape(), "g_ok "), "g_ok 1.5", "finite values still format as before");

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

Metrics.reset();
Metrics.counter("esc", 1, { path: 'a"b\\c' });
{
    const s = Metrics.scrape();
    ok(s.indexOf('path="a\\"b\\\\c"') >= 0, "quote and backslash escaped (" + line(s, "esc") + ")");
}

Metrics.reset();
threw(() => Metrics.counter(), /name is required/, "no name");
threw(() => Metrics.counter("has spaces"), /not a valid/, "a name with a space");
threw(() => Metrics.counter("9leading"), /not a valid/, "a name starting with a digit");
threw(() => Metrics.counter("bad-dash"), /not a valid/, "a name with a dash");
threw(() => Metrics.gauge("g"), /value is required/, "a gauge with no value");
threw(() => Metrics.counter("x", 1, 42), /must be an object/, "non-object labels");

Metrics.reset();
Metrics.counter("dual", 1);
threw(() => Metrics.gauge("dual", 1), /already registered as another type/,
      "the same series as two types");

Metrics.reset();
{
    let refusedAt = -1;
    for (let i = 0; i < 400; i++) {
        try { Metrics.counter("m" + i, 1); }
        catch (e) { refusedAt = i; ok(/full/.test(e.message), "the refusal says the registry is full"); break; }
    }
    ok(refusedAt > 0 && refusedAt <= 256,
       "a fixed registry refuses rather than growing (refused at " + refusedAt + ")");
    Metrics.counter("m0", 1);
    eq(line(Metrics.scrape(), "m0 "), "m0 2", "existing series keep working when full");
}

Metrics.reset();
Metrics.counter("f3", 1);
threw(function () { Metrics.counter("f3", Infinity); }, null, "counter refuses Infinity");
threw(function () { Metrics.counter("f3", NaN); }, null, "counter refuses NaN");
threw(function () { Metrics.counter("f3", 1e19); }, null, "counter refuses >= 2^63");
eq(line(Metrics.scrape(), "f3 "), "f3 1", "refused increments never touch the value");

threw(function () { Metrics.counter("f4", 1, { 'a"b': "c" }); }, null, "label name refuses a quote");
threw(function () { Metrics.counter("f4", 1, { "a b": "c" }); }, null, "label name refuses a space");
threw(function () { Metrics.counter("f4", 1, { "": "c" }); }, null, "label name refuses empty");
threw(function () { Metrics.counter("f4", 1, { "9bad": "c" }); }, null, "label name refuses a leading digit");
Metrics.counter("f4", 1, { ok_name: "v", _x2: "w" });
ok(Metrics.scrape().indexOf('f4{ok_name="v",_x2="w"} 1') >= 0, "valid label names render");

Metrics.reset();

Metrics.reset();
eq(Metrics.scrape(), "", "reset empties the registry");

if (fails) {
    print("test_metrics: " + fails + " FAILED of " + n);
    throw new Error("test_metrics failed");
}
print("test_metrics: " + n + " assertions, 0 failures");
