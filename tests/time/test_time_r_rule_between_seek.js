// covers: dyna:time.RRule.between
// between() and next() do not expand a rule from DTSTART: they seek to the period that holds the
// window start and expand from there. The seek used to start the generator AT the window start,
// which kept no phase: with INTERVAL > 1 the periods were counted from the window instead of from
// DTSTART, so YEARLY;INTERVAL=2 from 2026 returned 2027-12-25 and DAILY;INTERVAL=3, WEEKLY;
// INTERVAL=2, MONTHLY;INTERVAL=5, HOURLY;INTERVAL=5, MINUTELY;INTERVAL=7 all returned wrong dates.
// Now the seek steps a whole number of INTERVALs of the rule's own period (year, month, WKST week,
// day, hour, minute, second) from DTSTART; a COUNT rule still expands from DTSTART.
// Oracle: tests/lib/rrule_model.js, a brute-force expansion that shares no code with the engine
// (checked against python-dateutil 2.9.0 on these generators). It used to be all() on the same
// rule bounded by an UNTIL horizon, which could not see a fault that all() and between() share
// (a day mask both read): the model is compared with all() too. Random rules over
// every FREQ, INTERVAL 1..10, WKST, and BYMONTH/BYMONTHDAY/BYYEARDAY/BYWEEKNO/BYDAY(+n)/BYSETPOS
// sets; random windows (exclusive and inclusive), next() and prev() points.
// The window also bounds the day scan of a YEARLY or MONTHLY period (no COUNT, no BYSETPOS) and
// stops at the period holding the window end, and the year masks are built by walking months, not
// one civil-date conversion per day. Measured (mac, min of 5): helpdesk's one-day isHoliday window
// on a YEARLY rule 23.8 us -> 0.9 us; rows dominated by building the result Dates unchanged. The
// year masks are pinned by a hash of all() over 3000 random COUNT rules taken on the build before.
// Mutants (red): seek without the INTERVAL phase; seek on a COUNT rule; WEEKLY seek on Monday weeks
// regardless of WKST; MONTHLY seek one month late; the negative month-day mask off by one.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_between_seek");
import { RRule } from "dyna:time";
import { occurrences } from "../lib/rrule_model.js";

let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }

let seed = 20261115;
const rnd = (k) => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % k; };
const pick = (a) => a[rnd(a.length)];
const some = (a, max) => { const out = new Set(); for (let k = 1 + rnd(max); k > 0; k--) out.add(pick(a)); return [...out]; };
const FREQS = ["YEARLY", "MONTHLY", "WEEKLY", "DAILY", "HOURLY", "MINUTELY", "SECONDLY"];
const SPAN = { YEARLY: 80 * 365, MONTHLY: 25 * 365, WEEKLY: 6 * 365, DAILY: 3 * 365, HOURLY: 90, MINUTELY: 3, SECONDLY: 0.15 };
const WD = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"];
const DAY = 86400000;

function genRule() {
    const freq = pick(FREQS);
    const o = { freq, interval: 1 + rnd(10), dtstart: new Date(Date.UTC(2000 + rnd(30), rnd(12), 1 + rnd(28), rnd(24), rnd(60), rnd(60))) };
    if (rnd(3) === 0) o.wkst = pick(WD);
    const hi = freq === "HOURLY" || freq === "MINUTELY" || freq === "SECONDLY";
    const r = rnd(hi ? 6 : 9);
    if (r === 1) o.bymonth = some([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12], 4);
    if (r === 2) o.bymonthday = some([1, 2, 10, 15, 28, 29, 30, 31, -1, -2], 3);
    if (r === 3) o.byweekday = some(WD, 4);
    if (r === 4) { o.byweekday = some(WD, 3); o.bymonth = some([1, 3, 6, 9, 12], 2); }
    if (r === 5 && !hi) { o.byweekday = some(WD, 5); o.bysetpos = some([1, 2, -1, -2], 2); }
    if (r === 6 && (freq === "YEARLY" || freq === "MONTHLY")) o.byweekday = some(["1MO", "2TU", "-1FR", "3WE", "-2SU", "1SA"], 2);
    if (r === 7 && freq === "YEARLY") o.byyearday = some([1, 32, 100, 200, 365, 366, -1, -100], 3);
    if (r === 8 && freq === "YEARLY") { o.byweekno = some([1, 2, 20, 52, 53, -1], 2); if (rnd(2)) o.byweekday = some(WD, 2); }
    if (rnd(5) === 0) o.count = 1 + rnd(40);
    return o;
}

const iso = (a) => a.map((d) => d.toISOString()).join(",");
let rules = 0, windows = 0, points = 0, badBetween = 0, badNext = 0, badPrev = 0, multi = 0, badAll = 0, skipped = 0;
for (let t = 0; t < 600; t++) {
    const o = genRule();
    let r;
    try { r = new RRule(o); } catch (e) { skipped++; continue; }
    const ds = o.dtstart.getTime(), horizon = ds + SPAN[o.freq] * DAY;
    const occ = occurrences(o, horizon);
    if (o.count && occ.length < o.count) { skipped++; continue; }
    const oracle = occ.map((t) => new Date(t));
    let all;
    try { all = (o.count ? r.all() : new RRule({ ...o, until: new Date(horizon) }).all()).map((d) => d.getTime()); } catch (e) { all = ["E " + e.message]; }
    if (all.join() !== occ.join()) {
        badAll++;
        if (badAll <= 3) print("  all " + JSON.stringify(o) + "\n    want " + iso(oracle.slice(0, 3)) + "\n    got  " + String(all.slice(0, 3)));
    }
    rules++;
    if (o.interval > 1) multi++;
    for (let w = 0; w < 6; w++) {
        const a = ds - SPAN[o.freq] * DAY * 0.1 + rnd(1000) / 1000 * SPAN[o.freq] * DAY * 0.9;
        const b = Math.min(horizon, a + (rnd(1000) / 1000) * SPAN[o.freq] * DAY * 0.3);
        const inc = rnd(2) === 1;
        const s = new Date(Math.floor(a / 1000) * 1000), e = new Date(Math.floor(b / 1000) * 1000);
        const want = oracle.filter((d) => (inc ? d >= s && d <= e : d > s && d < e));
        let got;
        try { got = r.between(s, e, inc); } catch (x) { got = [new Date(0)]; }
        windows++;
        if (iso(got) !== iso(want)) {
            badBetween++;
            if (badBetween <= 3) print("  between " + JSON.stringify(o) + " " + s.toISOString() + ".." + e.toISOString() + " inc=" + inc + "\n    want " + iso(want.slice(0, 3)) + "\n    got  " + iso(got.slice(0, 3)));
        }
        const f = new Date(Math.floor(a / 1000) * 1000);
        const after = occ.find((x) => x > f.getTime());
        if (after !== undefined || o.count) {
            let nx;
            try { nx = r.next(f); } catch (x) { nx = new Date(0); }
            points++;
            if ((nx ? nx.getTime() : null) !== (after === undefined ? null : after)) {
                badNext++;
                if (badNext <= 3) print("  next " + JSON.stringify(o) + " from " + f.toISOString() + " want " + (after && new Date(after).toISOString()) + " got " + (nx && nx.toISOString()));
            }
        }
        const before = occ.filter((x) => x < f.getTime()).pop();
        if (f.getTime() < horizon) {
            let pv;
            try { pv = r.prev(f); } catch (x) { pv = new Date(0); }
            if ((pv ? pv.getTime() : null) !== (before === undefined ? null : before)) {
                badPrev++;
                if (badPrev <= 3) print("  prev " + JSON.stringify(o) + " from " + f.toISOString() + " want " + (before && new Date(before).toISOString()) + " got " + (pv && pv.toISOString()));
            }
        }
    }
}
print(`  ${rules} rules (${multi} with INTERVAL > 1, ${skipped} skipped), ${windows} windows, ${points} next() points`);
ok(rules === 560 && multi === 448 && skipped === 40, `the fixed seed runs 560 rules, 448 with INTERVAL > 1, and skips exactly 40 (refused by the constructor or a COUNT not reached by the horizon): ${rules}, ${multi}, ${skipped}`);
ok(badAll === 0, `all() equals the brute-force model on ${rules} random rules (${badAll} differ)`);
ok(badBetween === 0, `between() equals the filtered model on ${windows} random windows (${badBetween} differ)`);
ok(badNext === 0, `next() equals the first model occurrence after the point on ${points} points (${badNext} differ)`);
ok(badPrev === 0, `prev() equals the last model occurrence before the point (${badPrev} differ)`);
__kitS.section("YEARLY", () => {
    seed = 777;
    let h = 0, nr = 0, modelled = 0, modelBad = 0;
    for (let t = 0; t < 3000; t++) {
      const freq = pick(["YEARLY", "MONTHLY", "WEEKLY", "DAILY"]);
      const o = { freq, interval: 1 + rnd(4), dtstart: new Date(Date.UTC(1990 + rnd(40), rnd(12), 1 + rnd(28), rnd(24))), count: 1 + rnd(60) };
      if (rnd(2)) o.wkst = pick(WD);
      if (rnd(2)) o.bymonth = some([1,2,3,4,5,6,7,8,9,10,11,12], 4);
      if (rnd(2)) o.bymonthday = some([1,2,10,15,28,29,30,31,-1,-2,-31], 3);
      if (rnd(2)) o.byweekday = some(freq === "YEARLY" || freq === "MONTHLY" ? [...WD, "1MO", "-1FR", "2TU", "-2SU"] : WD, 4);
      if (freq === "YEARLY" && rnd(3) === 0) o.byyearday = some([1, 59, 60, 61, 365, 366, -1, -365], 3);
      if (freq === "YEARLY" && rnd(3) === 0) o.byweekno = some([1, 2, 52, 53, -1, 26], 2);
      if (rnd(4) === 0) o.bysetpos = some([1, -1, 2, 3], 2);
      let r; try { r = new RRule(o); } catch (e) { continue; }
      let s, got = null; try { got = r.all().map((d) => d.getTime()); s = got.map((t) => t / 1000).join(","); } catch (e) { s = "E" + e.message; }
      if (got && got.length && t % 4 === 0) {
          modelled++;
          const want = occurrences(o, got[got.length - 1]);
          if (want.join() !== got.join()) { modelBad++; if (modelBad <= 3) print("  all " + JSON.stringify(o) + " model " + want.slice(0, 3) + " got " + got.slice(0, 3)); }
      }
      for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0;
      nr++;
    }
    ok(modelled > 600 && modelBad === 0, `all() equals the brute-force model on ${modelled} COUNT rules (every fourth one) up to their last occurrence (${modelBad} differ)`);
    ok(nr === 3000 && h === 1979609609, `all() over 3000 random COUNT rules hashes as it did before the year masks were built incrementally (${nr}, ${h})`);
});
__kitS.section("YEARLY;INTERVAL=2 from 2026 has no 2027 occurrence", () => {
    const r = new RRule({ freq: "YEARLY", interval: 2, bymonth: [12], bymonthday: [25], dtstart: "2026-01-01T00:00:00Z" });
    ok(r.between(new Date("2027-01-01T00:00:00Z"), new Date("2028-01-01T00:00:00Z")).length === 0, "YEARLY;INTERVAL=2 from 2026 has no 2027 occurrence");
    ok(r.next(new Date("2027-01-01T00:00:00Z")).toISOString() === "2028-12-25T00:00:00.000Z", "and next() from 2027 is 2028-12-25");
});
__kitS.end();
print(`test_time_r_rule_between_seek: ${n - fails} passed, ${fails} failed`);
if (fails) throw new Error("test_time_r_rule_between_seek: " + fails + " failures");
