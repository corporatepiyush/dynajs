/* bench_time_dateparser.js -- what class DateParser costs.
 *
 * There is no crossover to publish, and pretending otherwise would be the
 * mistake CLAUDE.md section 14 records: the capability ships under clause (2),
 * because it EXPRESSES something no free function here could. There was no
 * natural-language date parser to be faster than.
 *
 * So what is measured is what a caller can act on: the one-off construction
 * cost, the per-parse cost of each form, and the cost of constructing per
 * parse instead of hoisting -- which is the only ratio that means anything
 * here, and it is the one that says whether hoisting matters.
 *
 * `#B <case> <ns_per_op>`.
 */
import { DateParser } from "dyna:time";

const NOW = 1785249000;

function bench(name, fn, reps) {
    let best = Infinity;
    for (let run = 0; run < 3; run++) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) fn();
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    print("#B " + name + " " + ((best * 1e6) / reps).toFixed(1));
    return (best * 1e6) / reps;
}

const p = new DateParser("en-US", { now: NOW });
bench("construct", () => new DateParser("en-US", { now: NOW }), 200000);
bench("parse_iso", () => p.parse("2026-07-28"), 200000);
bench("parse_iso_time", () => p.parse("2026-07-28T14:30:05"), 200000);
bench("parse_month_name", () => p.parse("28 July 2026"), 200000);
bench("parse_numeric", () => p.parse("07/28/2026"), 200000);
bench("parse_relative", () => p.parse("in 3 days"), 200000);
bench("parse_weekday", () => p.parse("next monday"), 200000);
/* The refusal path walks every alternative before giving up, so it is the
 * slowest thing the parser does and belongs in the published numbers. */
bench("parse_reject", () => p.parse("not a date at all"), 200000);

/* Hoisted versus constructed per call. The construction is a table pointer and
 * a malloc, so this ratio is expected to be small -- and a small number is the
 * honest answer, not a disappointing one. */
const hoisted = bench("hoisted_parse", () => p.parse("28 July 2026"), 200000);
const rebuilt = bench("rebuilt_parse",
    () => new DateParser("en-US", { now: NOW }).parse("28 July 2026"), 200000);
print("#R hoist_vs_rebuild " + (rebuilt / hoisted).toFixed(3));
