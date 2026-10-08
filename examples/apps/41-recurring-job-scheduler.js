// 41 · Recurring job scheduler — calendar rules (RFC 5545) decide when jobs run; a virtual clock tests a month in milliseconds.
//
// WHAT IT SHOWS
//   - dyna:time RRule: "every weekday at 09:00", "last day of the month", "every 15 minutes"
//   - one timer for the whole schedule: sleep until the earliest due job, not a tick per second
//   - catch-up policy after downtime: run once, not once per missed slot
//   - an injectable clock, which is what makes a scheduler testable at all
//
// RUN      dynajs examples/apps/41-recurring-job-scheduler.js

import { RRule } from "dyna:time";
import { Heap } from "dyna:structures";

class Scheduler {
    // `clock` supplies now() in ms and a sleep; production uses the real ones.
    constructor(clock = { now: () => Date.now(), sleep }) {
        this.clock = clock;
        this.jobs = new Map();
        // A heap keeps the next-due job on top without rescanning every job.
        this.queue = new Heap((a, b) => a.at - b.at);
        this.log = [];
    }

    // rule: an RRULE string, e.g. "FREQ=WEEKLY;BYDAY=MO,WE,FR;BYHOUR=..." is refused by
    // this engine (time-of-day comes from dtstart), so the start instant carries the time.
    add(name, ruleText, dtstart, fn) {
        const rule = RRule.fromString("RRULE:" + ruleText, { dtstart: new Date(dtstart) });
        const job = { name, rule, fn, runs: 0, skipped: 0 };
        this.jobs.set(name, job);
        this.#schedule(job, this.clock.now() - 1);
        return this;
    }

    #schedule(job, afterMs) {
        const next = job.rule.next(new Date(afterMs));     // first occurrence strictly after
        if (next) this.queue.push({ at: next.getTime(), job });
    }

    // Run until `untilMs` (or forever when omitted).
    async run(untilMs = Infinity) {
        for (;;) {
            const head = this.queue.peek();
            if (!head || head.at > untilMs) return;
            const wait = head.at - this.clock.now();
            if (wait > 0) await this.clock.sleep(wait);
            this.queue.pop();

            const { job, at } = head;
            const now = this.clock.now();
            // Catch-up: if the process was down (or a job overran) several
            // slots may be in the past. Run ONCE now and count the rest as
            // skipped; replaying 300 missed "every minute" runs helps nobody.
            let missed = 0;
            for (let probe = job.rule.next(new Date(at)); probe && probe.getTime() <= now; probe = job.rule.next(probe)) missed++;
            job.skipped += missed;
            try {
                await job.fn(new Date(at));
                job.runs++;
                this.log.push({ job: job.name, at });
            } catch (e) {
                // A failing job must not stop the scheduler or its own next run.
                this.log.push({ job: job.name, at, error: e.message });
            }
            this.#schedule(job, Math.max(at, now));
        }
    }
}

// ---- self-test on a virtual clock -------------------------------------------
// The clock only moves when the scheduler sleeps, so a simulated month runs
// instantly and deterministically.
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const start = Date.UTC(2026, 2, 1, 0, 0, 0);               // Sunday 1 March 2026
let virtualNow = start;
const clock = { now: () => virtualNow, sleep: async (ms) => { virtualNow += ms; } };
const iso = (ms) => new Date(ms).toISOString().slice(0, 16).replace("T", " ");

const sched = new Scheduler(clock);
const ran = { backup: [], report: [], invoice: [], flaky: 0 };
sched
    .add("backup", "FREQ=DAILY", Date.UTC(2026, 2, 1, 2, 30), (at) => ran.backup.push(at.getTime()))
    .add("report", "FREQ=WEEKLY;BYDAY=MO,WE,FR", Date.UTC(2026, 2, 2, 9, 0), (at) => ran.report.push(at.getTime()))
    .add("invoice", "FREQ=MONTHLY;BYMONTHDAY=-1", Date.UTC(2026, 2, 31, 18, 0), (at) => ran.invoice.push(at.getTime()))
    .add("flaky", "FREQ=DAILY", Date.UTC(2026, 2, 1, 12, 0), () => { ran.flaky++; throw new Error("upstream down"); });

await sched.run(Date.UTC(2026, 3, 1, 0, 0));               // the whole of March

check(ran.backup.length === 31, "the daily backup ran every day of March: " + ran.backup.length);
check(new Date(ran.backup[0]).getUTCHours() === 2 && new Date(ran.backup[0]).getUTCMinutes() === 30, "at 02:30");
check(ran.report.length === 13, "Mon/Wed/Fri reports: " + ran.report.length);
check(ran.report.every((t) => [1, 3, 5].includes(new Date(t).getUTCDay())), "only on the listed weekdays");
check(ran.invoice.length === 1 && iso(ran.invoice[0]) === "2026-03-31 18:00", "the last day of the month is found: " + ran.invoice.map(iso));
check(ran.flaky === 31 && sched.jobs.get("flaky").runs === 0, "a failing job keeps its schedule and is recorded as failed");
check(sched.log.every((e, i) => i === 0 || sched.log[i - 1].at <= e.at), "jobs fire in time order across rules");

// Downtime: a job due every 15 minutes, with a first run that takes two hours.
virtualNow = start;
const busy = new Scheduler(clock);
let calls = 0;
busy.add("poll", "FREQ=MINUTELY;INTERVAL=15", start, async () => { if (calls++ === 0) await clock.sleep(2 * 3600 * 1000); });
await busy.run(start + 4 * 3600 * 1000);
const poll = busy.jobs.get("poll");
check(poll.skipped >= 7 && poll.runs < 16, `missed slots are skipped, not replayed (runs ${poll.runs}, skipped ${poll.skipped})`);

console.log("March 2026: backup x" + ran.backup.length, "| report x" + ran.report.length, "| invoice", iso(ran.invoice[0]));
console.log("self-test passed");
