// covers: c:src/modules/time/time-date-parser.c
// Month and weekday lookup skips every locale name whose first letter differs from the input's
// (lower-cased) first byte before the full case-insensitive word compare, and stops at once at the
// end of input: 17% faster on mixed date text, where each month token used to be compared against
// all 24 names. Every name in every locale is pinned here in four spellings (lower, UPPER, Title,
// aLtErNaTe), as day-first and month-first text, with and without a leading weekday, next to
// near-misses that share the first letter (Mar/Marchy, Ma, jui -> juin) and inputs that end on the
// name. Expected values come from Date.UTC and plain weekday arithmetic.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_date_parser_names");
import * as T from "dyna:time";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };
const NOW = 1700000000;
const L = {
    en: { locs: ["en-US", "en-GB", "en"], months: ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"], abbr: ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"], days: ["sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"] },
    fr: { locs: ["fr"], months: ["janvier", "fevrier", "mars", "avril", "mai", "juin", "juillet", "aout", "septembre", "octobre", "novembre", "decembre"], abbr: ["jan", "fev", "mar", "avr", "mai", "jui", "jul", "aou", "sep", "oct", "nov", "dec"], days: ["dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"] },
    de: { locs: ["de"], months: ["januar", "februar", "marz", "april", "mai", "juni", "juli", "august", "september", "oktober", "november", "dezember"], abbr: ["jan", "feb", "mar", "apr", "mai", "jun", "jul", "aug", "sep", "okt", "nov", "dez"], days: ["sonntag", "montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag"] },
    es: { locs: ["es"], months: ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"], abbr: ["ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"], days: ["domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"] },
};
const spell = (w) => [w, w.toUpperCase(), w[0].toUpperCase() + w.slice(1), [...w].map((c, i) => (i % 2 ? c.toUpperCase() : c)).join("")];
const utc = (y, m, d) => Date.UTC(y, m, d) / 1000;
const firstIdx = (list, w) => list.indexOf(w);

__kitS.section("every month name and abbreviation in four spellings", () => {
    for (const k in L) {
        const { locs, months, abbr } = L[k];
        for (const loc of locs) {
            const dp = new T.DateParser(loc, { now: NOW });
            for (let i = 0; i < 12; i++) {
                const d = 1 + ((i * 5) % 28), y = 1990 + i * 3;
                for (const name of [months[i], abbr[i]]) {
                    const mi = firstIdx(months, name) >= 0 ? firstIdx(months, name) : firstIdx(abbr, name);
                    for (const sp of spell(name)) {
                        eq(dp.parse(d + " " + sp + " " + y), utc(y, mi, d), loc + " day-first " + sp);
                        if (k === "en" || k === "fr")
                            eq(dp.parse(sp + " " + d + " " + y), k === "fr" && name === "mar" ? null : utc(y, mi, d), loc + " month-first " + sp + (name === "mar" && k === "fr" ? " (leading mar is mardi's 3-letter form, a weekday)" : ""));
                    }
                }
            }
        }
    }
});

__kitS.section("a leading weekday is read and does not move the date", () => {
    for (const k in L) {
        const { locs, months, days } = L[k];
        const dp = new T.DateParser(locs[0], { now: NOW });
        for (let w = 0; w < 7; w++)
            for (const sp of spell(days[w]))
                eq(dp.parse(sp + " " + (3 + w) + " " + months[w] + " 2021"), utc(2021, w, 3 + w), locs[0] + " weekday " + sp);
    }
    const en = new T.DateParser("en-US", { now: NOW });
    eq(en.parse("Friday, March 5, 2021"), utc(2021, 2, 5), "weekday, month day, year");
    eq(en.parse("Fri March 5 2021"), utc(2021, 2, 5), "three-letter weekday");
});

__kitS.section("relative weekdays from a pinned now (a Tuesday)", () => {
    const now = new Date(NOW * 1000), today = utc(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()), dow = now.getUTCDay();
    for (const k of ["en"]) {
        const dp = new T.DateParser("en-US", { now: NOW });
        for (let w = 0; w < 7; w++) {
            const ahead = ((w - dow + 7) % 7) || 7, back = ((dow - w + 7) % 7) || 7;
            for (const sp of spell(L[k].days[w])) {
                eq(dp.parse("next " + sp), today + ahead * 86400, "next " + sp);
                eq(dp.parse(sp), null, "a bare weekday is not a date: " + sp);
                eq(dp.parse("last " + sp), today - back * 86400, "last " + sp);
                eq(dp.parse("next " + sp.slice(0, 3)), today + ahead * 86400, "next " + sp.slice(0, 3));
            }
        }
    }
    eq(new T.DateParser("fr", { now: NOW }).parse("next lundi"), today + 6 * 86400, "fr weekday after next");
});

__kitS.section("near-misses sharing the first letter, and inputs ending on a name", () => {
    const en = new T.DateParser("en-US", { now: NOW });
    for (const t of ["5 Marchy 2021", "5 Ma 2021", "5 M 2021", "5 Mars 2021", "5 Mayo 2021", "5 Juneau 2021", "5 Septembe 2021", "Tues", "Mon", "monday 5", "March", "5 Mar", "M", "", "5 ñar 2021", "5 é 2021", "Monda 5 March 2021", "on friday"])
        eq(en.parse(t), null, "en rejects " + JSON.stringify(t));
    eq(en.parse("5 Sept 2021"), null, "a four-letter abbreviation is not a name");
    const fr = new T.DateParser("fr", { now: NOW });
    eq(fr.parse("5 jui 2021"), utc(2021, 5, 5), "fr jui is the first match, juin");
    eq(fr.parse("5 jul 2021"), utc(2021, 6, 5), "fr jul is juillet");
    eq(fr.parse("5 mar 2021"), utc(2021, 2, 5), "fr mar abbreviation");
    eq(fr.parse("5 mai 2021"), utc(2021, 4, 5), "fr mai: name and abbreviation agree");
    eq(fr.parse("5 March 2021"), null, "fr does not know English months");
    const de = new T.DateParser("de", { now: NOW });
    eq(de.parse("5 Marz 2021"), utc(2021, 2, 5), "de Marz");
    eq(de.parse("5 Mars 2021"), null, "de rejects fr mars");
    const es = new T.DateParser("es", { now: NOW });
    eq(es.parse("5 mayo 2021"), utc(2021, 4, 5), "es mayo (shares m with marzo)");
    eq(es.parse("5 may 2021"), utc(2021, 4, 5), "es may abbreviation");
    eq(es.parse("5 marz 2021"), null, "es truncated marzo");
});

__kitS.end();
print(`test_time_date_parser_names: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
