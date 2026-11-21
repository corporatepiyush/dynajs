// Brute-force RRULE model: an oracle for dyna:time RRule that shares no code with it.
//
// It walks every period from DTSTART in steps of INTERVAL (year, month, WKST week, day, hour,
// minute, second), lists every candidate instant in the period, keeps the ones every BYxxx filter
// accepts, applies BYSETPOS to the period's sorted set, then drops instants before DTSTART and
// stops at COUNT, UNTIL or the horizon. No seeking and no incremental state: one civil-date
// conversion per candidate day. Semantics are python-dateutil's (the reference the engine is held
// to in test_time_r_rule_dateutil_golden): the DTSTART defaults for a rule with no day filter,
// ordinal BYDAY counted within the month for MONTHLY and for YEARLY with BYMONTH, else within the
// year, a day that must pass BOTH the plain weekdays and the ordinal ones when a BYDAY mixes them,
// and BYWEEKNO by WKST weeks where week 1 holds at least 4 days of its year (a day in the
// next year's week 1 matches 1; a day in the previous year's last week matches -1 or the number
// dateutil computes for it, quirk included), and a WEEKLY rule's first period running from the
// DTSTART day, not the WKST day, to the end of its week (so BYSETPOS counts only those days).
// It was checked against dateutil 2.9.0 before use: 3200 rules of the between_seek generator
// (4 seeds) and the 3000 COUNT rules of its second section, all equal.
//
// occurrences(o, horizonMs) -> sorted epoch-ms list; o is the RRule options object with Date
// dtstart/until, upper-case freq and weekday strings ("MO", "1MO", "-1FR").
const DAY = 86400000;
const WD = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"];
const FREQ_RANK = { YEARLY: 0, MONTHLY: 1, WEEKLY: 2, DAILY: 3, HOURLY: 4, MINUTELY: 5, SECONDLY: 6 };

function civil(day) {
    const z = day + 719468, era = Math.floor(z / 146097), doe = z - era * 146097;
    const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
    const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100)), mp = Math.floor((5 * doy + 2) / 153);
    const m = mp < 10 ? mp + 3 : mp - 9;
    return { y: yoe + era * 400 + (m <= 2 ? 1 : 0), m, d: doy - Math.floor((153 * mp + 2) / 5) + 1, wd: ((day % 7) + 7 + 3) % 7 };
}
function dayOf(y, m, d) {
    y -= m <= 2 ? 1 : 0;
    const era = Math.floor(y / 400), yoe = y - era * 400;
    const doy = Math.floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + d - 1;
    return era * 146097 + yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy - 719468;
}
const isLeap = (y) => (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0;
const yearLen = (y) => (isLeap(y) ? 366 : 365);
const monthLen = (y, m) => (m === 2 ? (isLeap(y) ? 29 : 28) : [4, 6, 9, 11].includes(m) ? 30 : 31);

function weeknoMask(y, list, wkst) {
    const yl = yearLen(y), jan1 = dayOf(y, 1, 1), ywd = civil(jan1).wd;
    const wdOf = (i) => (ywd + i) % 7;
    const mask = new Set();
    const firstwkst = (7 - ywd + wkst) % 7;
    let no1wkst, wyearlen;
    if (firstwkst >= 4) { no1wkst = 0; wyearlen = yl + ((ywd - wkst + 7) % 7); }
    else { no1wkst = firstwkst; wyearlen = yl - no1wkst; }
    const numweeks = Math.floor(wyearlen / 7) + Math.floor((wyearlen % 7) / 4);
    const markWeek = (i) => { for (let j = 0; j < 7; j++) { mask.add(i); i++; if (wdOf(i) === wkst) break; } };
    for (let n of list) {
        if (n < 0) n += numweeks + 1;
        if (!(n > 0 && n <= numweeks)) continue;
        let i;
        if (n > 1) { i = no1wkst + (n - 1) * 7; if (no1wkst !== firstwkst) i -= 7 - firstwkst; }
        else i = no1wkst;
        markWeek(i);
    }
    if (list.includes(1)) {
        let i = no1wkst + numweeks * 7;
        if (no1wkst !== firstwkst) i -= 7 - firstwkst;
        if (i < yl) markWeek(i);
    }
    if (no1wkst) {
        let lnumweeks;
        if (!list.includes(-1)) {
            const lywd = civil(dayOf(y - 1, 1, 1)).wd, lyl = yearLen(y - 1);
            const lno1wkst = (7 - lywd + wkst) % 7;
            if (lno1wkst >= 4) lnumweeks = 52 + Math.floor(((lyl + ((lywd - wkst + 7) % 7)) % 7) / 4);
            else lnumweeks = 52 + Math.floor(((yl - no1wkst) % 7) / 4);
        } else lnumweeks = -1;
        if (list.includes(lnumweeks)) for (let i = 0; i < no1wkst; i++) mask.add(i);
    }
    return mask;
}
const maskCache = new Map();
function weeknoMatches(day, y, list, wkst) {
    let mk = maskCache.get(y);
    if (!mk) { mk = { first: dayOf(y, 1, 1), mask: weeknoMask(y, list, wkst) }; maskCache.set(y, mk); }
    return mk.mask.has(day - mk.first);
}

function parseDays(list) {
    return (list || []).map((s) => {
        const m = /^([+-]?\d+)?(MO|TU|WE|TH|FR|SA|SU)$/.exec(s);
        return { n: m[1] ? Number(m[1]) : 0, wd: WD.indexOf(m[2]) };
    });
}

export function occurrences(o, horizonMs) {
    maskCache.clear();
    const freq = o.freq, rank = FREQ_RANK[freq], I = o.interval || 1;
    const ds = o.dtstart.getTime(), dsDay = Math.floor(ds / DAY), c0 = civil(dsDay);
    const tod = ds - dsDay * DAY;
    const wkst = WD.indexOf(o.wkst || "MO");
    let bymonth = o.bymonth || null, bymonthday = o.bymonthday || null, byday = parseDays(o.byweekday);
    const byyearday = o.byyearday || null, byweekno = o.byweekno || null, setpos = o.bysetpos || null;
    if (!byweekno && !byyearday && !bymonthday && !byday.length) {
        if (freq === "YEARLY") { if (!bymonth) bymonth = [c0.m]; bymonthday = [c0.d]; }
        else if (freq === "MONTHLY") bymonthday = [c0.d];
        else if (freq === "WEEKLY") byday = [{ n: 0, wd: c0.wd }];
    }
    if (rank > 1) byday = byday.map((b) => ({ n: 0, wd: b.wd }));
    const until = o.until ? o.until.getTime() : Infinity, count = o.count || Infinity;
    const end = Math.min(until, horizonMs);

    let yfY = NaN, yfD = 0;
    const yearFirst = (y) => (y === yfY ? yfD : (yfY = y, yfD = dayOf(y, 1, 1)));
    const plain = byday.filter((b) => b.n === 0).map((b) => b.wd), nth = byday.filter((b) => b.n !== 0);
    const nthInMonth = freq === "MONTHLY" || (freq === "YEARLY" && !!bymonth);
    function dayOk(day, y, m, d, wd) {
        if (bymonth && !bymonth.includes(m)) return false;
        if (byweekno && !weeknoMatches(day, y, byweekno, wkst)) return false;
        if (byyearday) {
            const doy = day - yearFirst(y) + 1, yl = yearLen(y);
            if (!byyearday.some((n) => n === doy || n === doy - yl - 1)) return false;
        }
        if (bymonthday) {
            const ml = monthLen(y, m);
            if (!bymonthday.some((n) => n === d || n === d - ml - 1)) return false;
        }
        if (plain.length && !plain.includes(wd)) return false;
        if (nth.length) {
            const first = nthInMonth ? dayOf(y, m, 1) : yearFirst(y);
            const last = nthInMonth ? first + monthLen(y, m) - 1 : first + yearLen(y) - 1;
            const k = Math.floor((day - first) / 7) + 1, kb = -(Math.floor((last - day) / 7) + 1);
            if (!nth.some((b) => b.wd === wd && (b.n === k || b.n === kb))) return false;
        }
        return true;
    }
    let kdDay = NaN, kdOk = false;
    const keepDay = (day) => { if (day !== kdDay) { const c = civil(day); kdDay = day; kdOk = dayOk(day, c.y, c.m, c.d, c.wd); } return kdOk; };
    function monthDays(y, m, into) {
        if (bymonth && !bymonth.includes(m)) return;
        const first = dayOf(y, m, 1), n = monthLen(y, m);
        const wd1 = civil(first).wd;
        const days = bymonthday ? [...new Set(bymonthday.map((k) => (k > 0 ? k : n + k + 1)))].filter((d) => d >= 1 && d <= n).sort((a, b) => a - b) : null;
        for (let i = 0, d; (d = days ? days[i] : i + 1) !== undefined && d <= n; i++) if (dayOk(first + d - 1, y, m, d, (wd1 + d - 1) % 7)) into.push((first + d - 1) * DAY + tod);
    }

    const out = [];
    for (let p = 0; ; p++) {
        let set = [], start;
        if (freq === "YEARLY") {
            const y = c0.y + p * I;
            start = dayOf(y, 1, 1) * DAY;
            if (start > end) break;
            for (let m = 1; m <= 12; m++) monthDays(y, m, set);
        } else if (freq === "MONTHLY") {
            const mi = (c0.y * 12 + c0.m - 1) + p * I, y = Math.floor(mi / 12), m = mi % 12 + 1;
            start = dayOf(y, m, 1) * DAY;
            if (start > end) break;
            monthDays(y, m, set);
        } else if (freq === "WEEKLY") {
            const ws = dsDay - ((c0.wd - wkst + 7) % 7) + p * 7 * I;
            start = ws * DAY;
            if (start > end) break;
            for (let d = p === 0 ? dsDay : ws; d < ws + 7; d++) if (keepDay(d)) set.push(d * DAY + tod);
        } else {
            const unit = { DAILY: DAY, HOURLY: 3600000, MINUTELY: 60000, SECONDLY: 1000 }[freq];
            start = Math.floor(ds / unit) * unit + p * I * unit;
            if (start > end) break;
            const t = start + (ds - Math.floor(ds / unit) * unit);
            if (keepDay(Math.floor(t / DAY))) set.push(t);
        }
        if (setpos) {
            const pick = [];
            for (const n of setpos) {
                const i = n > 0 ? n - 1 : set.length + n;
                if (i >= 0 && i < set.length && !pick.includes(set[i])) pick.push(set[i]);
            }
            set = pick.sort((a, b) => a - b);
        }
        for (const t of set) {
            if (t < ds) continue;
            if (t > end) return out;
            out.push(t);
            if (out.length >= count) return out;
        }
    }
    return out;
}
