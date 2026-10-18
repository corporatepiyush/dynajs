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
bench("parse_reject", () => p.parse("not a date at all"), 200000);

const hoisted = bench("hoisted_parse", () => p.parse("28 July 2026"), 200000);
const rebuilt = bench("rebuilt_parse",
    () => new DateParser("en-US", { now: NOW }).parse("28 July 2026"), 200000);
print("#R hoist_vs_rebuild " + (rebuilt / hoisted).toFixed(3));
