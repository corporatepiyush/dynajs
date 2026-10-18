// Astronomy — positional astronomy from first principles (Julian dates, sidereal time, Kepler's equation,
// coordinate transforms, rise/set with refraction, the Moon's phase), an observing planner for a deep-sky
// catalogue, and an exoplanet transit search with a Box Least Squares periodogram.
//
// WHAT IT IS
//   time         Julian Day from calendar dates and Greenwich mean sidereal time, checked against the worked
//                examples in Meeus, "Astronomical Algorithms" (2nd ed.), examples 7.a and 12.a
//   planets      heliocentric positions from J2000 orbital elements with secular rates: Kepler's equation solved by
//                Newton's method, then heliocentric -> geocentric -> equatorial (RA/Dec) -> horizontal (alt/az)
//                for an observer; Venus's elongation and Mars's distance are reported
//   Sun & Moon   low-precision solar and lunar positions, the Moon's illuminated fraction and phase angle
//   rise/set     iterative rise, transit and set times with standard refraction (−0.5667°) for each target
//   planner      for a catalogue of Messier objects (CSV): visible tonight above 30° during astronomical darkness,
//                best time, airmass, separation from the Moon, ranked by a score
//   transits     a TESS-like light curve (2-minute cadence, 27 days, noise, stellar variability) with a hidden
//                planet; detrended with a running median and searched with Box Least Squares over periods and
//                durations; the period, depth and duration are recovered and the planet radius estimated
//   sky index    the catalogue's directions are unit vectors in a KDTree: "everything within 25° of the Moon" is a
//                radius query with the chord length 2·sin(θ/2), not a separation computed against every object
//
// MODULES  dyna:mathx (stats) · dyna:dataframe (ROLLING_MEDIAN-like via QUANTILE) · dyna:csv · dyna:time (formatRFC3339)
//          · dyna:random · dyna:cli (Table) · dyna:simd (sum, f64Mean) · dyna:file · dyna:structures (KDTree)
//
// RUN      dynajs examples/science/astronomy.js     plans a night at La Palma and searches a light curve for a planet

import { stats } from "dyna:mathx";
import { DataFrame } from "dyna:dataframe";
import * as csv from "dyna:csv";
import { formatRFC3339 } from "dyna:time";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { sum, f64Mean } from "dyna:simd";
import { KDTree } from "dyna:structures";
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("astro");
const rng = new Random(1995);
const D2R = Math.PI / 180,
    R2D = 180 / Math.PI;
const norm360 = (x) => ((x % 360) + 360) % 360;

// =============================================================================
// Time
// =============================================================================
function julianDay(y, m, d) {
    // Meeus ch. 7, Gregorian calendar
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    const A = Math.floor(y / 100),
        B = 2 - A + Math.floor(A / 4);
    return Math.floor(365.25 * (y + 4716)) + Math.floor(30.6001 * (m + 1)) + d + B - 1524.5;
}
const jdFromDate = (date) => date.getTime() / 86400000 + 2440587.5;
const dateFromJd = (jd) => new Date((jd - 2440587.5) * 86400000);
function gmst(jd) {
    // Meeus eq. 12.4, degrees
    const T = (jd - 2451545.0) / 36525;
    return norm360(280.46061837 + 360.98564736629 * (jd - 2451545) + 0.000387933 * T * T - (T * T * T) / 38710000);
}

// =============================================================================
// Orbits (J2000 elements and rates per century; JPL "Keplerian Elements for Approximate Positions")
// =============================================================================
const ELEMENTS = {
    Mercury: [
        0.38709927, 0.20563593, 7.00497902, 252.2503235, 77.45779628, 48.33076593, 0.00000037, 0.00001906, -0.00594749,
        149472.67411175, 0.16047689, -0.12534081,
    ],
    Venus: [
        0.72333566, 0.00677672, 3.39467605, 181.9790995, 131.60246718, 76.67984255, 0.0000039, -0.00004107, -0.0007889,
        58517.81538729, 0.00268329, -0.27769418,
    ],
    Earth: [
        1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0, 0.00000562, -0.00004392, -0.01294668,
        35999.37244981, 0.32327364, 0,
    ],
    Mars: [
        1.52371034, 0.0933941, 1.84969142, -4.55343205, -23.94362959, 49.55953891, 0.00001847, 0.00007882, -0.00813131,
        19140.30268499, 0.44441088, -0.29257343,
    ],
    Jupiter: [
        5.202887, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909, -0.00011607, -0.00013253, -0.00183714,
        3034.74612775, 0.21252668, 0.20469106,
    ],
    Saturn: [
        9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448, -0.0012506, -0.00050991, 0.00193609,
        1222.49362201, -0.41897216, -0.28867794,
    ],
};
function helio(name, jd) {
    const T = (jd - 2451545) / 36525,
        e0 = ELEMENTS[name],
        el = e0.slice(0, 6).map((v, i) => v + e0[i + 6] * T);
    const [a, e, I, L, wbar, node] = el,
        w = wbar - node;
    let M = norm360(L - wbar);
    if (M > 180) M -= 360;
    let E = M * D2R;
    for (let k = 0; k < 30; k++) {
        const dE = (E - e * Math.sin(E) - M * D2R) / (1 - e * Math.cos(E));
        E -= dE;
        if (Math.abs(dE) < 1e-12) break;
    } // Kepler by Newton
    const xp = a * (Math.cos(E) - e),
        yp = a * Math.sqrt(1 - e * e) * Math.sin(E);
    const [cw, sw, cn, sn, ci, si] = [
        Math.cos(w * D2R),
        Math.sin(w * D2R),
        Math.cos(node * D2R),
        Math.sin(node * D2R),
        Math.cos(I * D2R),
        Math.sin(I * D2R),
    ];
    return [
        (cw * cn - sw * sn * ci) * xp + (-sw * cn - cw * sn * ci) * yp,
        (cw * sn + sw * cn * ci) * xp + (-sw * sn + cw * cn * ci) * yp,
        sw * si * xp + cw * si * yp,
    ];
}
const EPS = 23.43928 * D2R;
const eclToEq = ([x, y, z]) => [x, y * Math.cos(EPS) - z * Math.sin(EPS), y * Math.sin(EPS) + z * Math.cos(EPS)];
const toRaDec = ([x, y, z]) => ({
    ra: norm360(Math.atan2(y, x) * R2D),
    dec: Math.atan2(z, Math.hypot(x, y)) * R2D,
    dist: Math.hypot(x, y, z),
});
function planet(name, jd) {
    const p = helio(name, jd),
        e = helio("Earth", jd);
    return toRaDec(eclToEq([p[0] - e[0], p[1] - e[1], p[2] - e[2]]));
}
function sun(jd) {
    const e = helio("Earth", jd);
    return toRaDec(eclToEq([-e[0], -e[1], -e[2]]));
}
function moon(jd) {
    // low-precision lunar theory (a few arc-minutes)
    const d = jd - 2451545,
        L = norm360(218.316 + 13.176396 * d),
        M = norm360(134.963 + 13.064993 * d),
        F = norm360(93.272 + 13.22935 * d);
    const lon = (L + 6.289 * Math.sin(M * D2R)) * D2R,
        lat = 5.128 * Math.sin(F * D2R) * D2R,
        dist = 385001 - 20905 * Math.cos(M * D2R);
    return {
        ...toRaDec(eclToEq([Math.cos(lat) * Math.cos(lon), Math.cos(lat) * Math.sin(lon), Math.sin(lat)])),
        km: dist,
    };
}
const separation = (a, b) =>
    Math.acos(
        Math.min(
            1,
            Math.sin(a.dec * D2R) * Math.sin(b.dec * D2R) +
                Math.cos(a.dec * D2R) * Math.cos(b.dec * D2R) * Math.cos((a.ra - b.ra) * D2R),
        ),
    ) * R2D;
function horizontal(obj, jd, site) {
    const H = norm360(gmst(jd) + site.lon - obj.ra) * D2R,
        dec = obj.dec * D2R,
        lat = site.lat * D2R;
    const alt = Math.asin(Math.sin(lat) * Math.sin(dec) + Math.cos(lat) * Math.cos(dec) * Math.cos(H));
    const az = Math.atan2(
        -Math.cos(dec) * Math.sin(H),
        Math.sin(dec) * Math.cos(lat) - Math.cos(dec) * Math.sin(lat) * Math.cos(H),
    );
    return { alt: alt * R2D, az: norm360(az * R2D) };
}
const airmass = (alt) => (alt <= 0 ? Infinity : 1 / (Math.sin(alt * D2R) + 0.50572 * (alt + 6.07995) ** -1.6364)); // Kasten & Young 1989
function riseSet(posAt, jd0, site, h0 = -0.5667) {
    // scan in 10-minute steps, refine by bisection
    const altAt = (jd) => horizontal(posAt(jd), jd, site).alt - h0,
        out = {};
    let prevJd = jd0,
        prev = altAt(jd0),
        best = { alt: -Infinity };
    for (let k = 1; k <= 144; k++) {
        const jd = jd0 + k / 144,
            a = altAt(jd);
        if (a + h0 > best.alt) best = { alt: a + h0, jd };
        if (prev < 0 !== a < 0) {
            let lo = prevJd,
                hi = jd;
            for (let i = 0; i < 30; i++) {
                const mid = (lo + hi) / 2;
                altAt(mid) < 0 === prev < 0 ? (lo = mid) : (hi = mid);
            }
            out[prev < 0 ? "rise" : "set"] ??= (lo + hi) / 2;
        }
        prevJd = jd;
        prev = a;
    }
    out.transit = best.jd;
    out.maxAlt = best.alt;
    return out;
}

// =============================================================================
// Night plan for La Palma
// =============================================================================
const SITE = { name: "Roque de los Muchachos", lat: 28.7606, lon: -17.8816 };
const NIGHT = Date.UTC(2026, 8, 12, 18, 0); // 12 September 2026, from 18:00 UT
const jdNight = jdFromDate(new Date(NIGHT));
const darkStart = (() => {
    for (let k = 0; k < 144; k++) {
        const jd = jdNight + k / 144;
        if (horizontal(sun(jd), jd, SITE).alt < -18) return jd;
    }
    return null;
})();
const darkEnd = (() => {
    for (let k = 1; k < 144; k++) {
        const jd = darkStart + k / 144;
        if (horizontal(sun(jd), jd, SITE).alt > -18) return jd;
    }
    return null;
})();
const mid = (darkStart + darkEnd) / 2,
    sunMid = sun(mid),
    moonMid = moon(mid);
const phaseAngle = separation(sunMid, moonMid),
    illum = (1 - Math.cos(phaseAngle * D2R)) / 2;
writeFile(
    work.join("messier.csv"),
    csv.stringify([
        ["id", "name", "type", "ra_deg", "dec_deg", "mag"],
        ["M13", "Hercules Cluster", "globular", "250.42", "36.46", "5.8"],
        ["M31", "Andromeda Galaxy", "galaxy", "10.68", "41.27", "3.4"],
        ["M42", "Orion Nebula", "nebula", "83.82", "-5.39", "4.0"],
        ["M57", "Ring Nebula", "planetary", "283.40", "33.03", "8.8"],
        ["M27", "Dumbbell Nebula", "planetary", "299.90", "22.72", "7.5"],
        ["M8", "Lagoon Nebula", "nebula", "270.92", "-24.38", "6.0"],
        ["M51", "Whirlpool Galaxy", "galaxy", "202.47", "47.20", "8.4"],
        ["M45", "Pleiades", "open cluster", "56.75", "24.12", "1.6"],
        ["M104", "Sombrero Galaxy", "galaxy", "189.99", "-11.62", "8.0"],
        ["M11", "Wild Duck Cluster", "open cluster", "282.77", "-6.27", "5.8"],
        ["M33", "Triangulum Galaxy", "galaxy", "23.46", "30.66", "5.7"],
        ["M81", "Bode's Galaxy", "galaxy", "148.89", "69.07", "6.9"],
    ]),
);
const catalog = csv
    .parse(readFile(work.join("messier.csv")), { hasHeader: true })
    .rows.map(([id, name, type, ra, dec, mag]) => ({
        id,
        name,
        type,
        ra: Number(ra),
        dec: Number(dec),
        mag: Number(mag),
    }));
const plan = catalog
    .map((o) => {
        let best = { alt: -90 };
        for (let jd = darkStart; jd <= darkEnd; jd += 1 / 96) {
            const h = horizontal(o, jd, SITE);
            if (h.alt > best.alt) best = { ...h, jd };
        }
        const moonSep = separation(o, moon(best.jd ?? mid));
        const score =
            best.alt < 30
                ? 0
                : (best.alt / 90) * (1 - illum * Math.max(0, 1 - moonSep / 90)) * (10 - Math.min(9, o.mag));
        return { ...o, best, airmass: airmass(best.alt), moonSep, score };
    })
    .sort((a, b) => b.score - a.score);
const unitVec = (o) => [
    Math.cos(o.dec * D2R) * Math.cos(o.ra * D2R),
    Math.cos(o.dec * D2R) * Math.sin(o.ra * D2R),
    Math.sin(o.dec * D2R),
];
const sky = new KDTree(catalog.map(unitVec));
const MOON_GLARE = 25;
const nearMoon = [...sky.radius(unitVec(moonMid), 2 * Math.sin((MOON_GLARE * D2R) / 2)).indices]
    .map((i) => catalog[i].id)
    .sortBy((x) => x);
const planets = ["Venus", "Mars", "Jupiter", "Saturn"].map((p) => {
    const pos = planet(p, mid),
        rs = riseSet((jd) => planet(p, jd), jdNight - 0.25, SITE),
        elong = separation(pos, sunMid);
    return { name: p, ...pos, rs, elong, alt: horizontal(pos, mid, SITE).alt };
});

// =============================================================================
// Transit search: Box Least Squares
// =============================================================================
const CAD = 2 / 1440,
    DAYS = 27,
    TRUE = { period: 3.8521, t0: 1.317, depth: 0.0021, duration: 0.118 };
const t = [],
    flux = [];
for (let i = 0; i * CAD < DAYS; i++) {
    const ti = i * CAD;
    if (ti > 13.2 && ti < 14.1) continue; // the data-downlink gap
    const phase = (((ti - TRUE.t0) % TRUE.period) + TRUE.period) % TRUE.period,
        inTransit = phase < TRUE.duration / 2 || phase > TRUE.period - TRUE.duration / 2;
    const variability = 0.003 * Math.sin((2 * Math.PI * ti) / 5.7) + 0.0012 * Math.sin((2 * Math.PI * ti) / 1.9 + 1);
    t.push(ti);
    flux.push(1 + variability - (inTransit ? TRUE.depth : 0) + rng.normal(0, 0.0009));
}
const W = Math.round(0.75 / CAD); // running median over 18 hours removes the star's variability
const STEP = 15,
    medians = []; // the median on a coarse grid is enough for a trend this slow
for (let i = 0; i < flux.length; i += STEP) {
    const lo = Math.max(0, i - (W >> 1));
    medians.push(stats.median(flux.slice(lo, lo + W)));
}
const detrended = flux.map((f, i) => f / medians[Math.min(medians.length - 1, Math.round(i / STEP))]);
function bls(periods, durations, nbins = 400) {
    const results = [];
    const meanF = f64Mean(Float64Array.from(detrended));
    for (const P of periods) {
        const binSum = new Float64Array(nbins),
            binN = new Float64Array(nbins);
        for (let i = 0; i < t.length; i++) {
            const b = Math.floor(((t[i] % P) / P) * nbins);
            binSum[b] += detrended[i] - meanF;
            binN[b]++;
        }
        let best = { power: 0 };
        const cs = new Float64Array(2 * nbins + 1),
            cn = new Float64Array(2 * nbins + 1); // prefix sums over two wraps
        for (let b = 0; b < 2 * nbins; b++) {
            cs[b + 1] = cs[b] + binSum[b % nbins];
            cn[b + 1] = cn[b] + binN[b % nbins];
        }
        for (const dur of durations) {
            const w = Math.max(1, Math.round((dur / P) * nbins));
            for (let s = 0; s < nbins; s++) {
                const sr = cs[s + w] - cs[s],
                    n = cn[s + w] - cn[s];
                if (n < 5) continue;
                const r = n / t.length,
                    power = (sr * sr) / (t.length * r * (1 - r));
                if (power > best.power) best = { power, P, dur, depth: -sr / n, epoch: ((s + w / 2) / nbins) * P };
            }
        }
        results.push(best);
    }
    return results;
}
const periods = Array.from({ length: 1000 }, (_, i) => 1 + i * 0.0067);
const periodogram = bls(periods, [0.06, 0.09, 0.12, 0.16]);
const top = periodogram.reduce((a, b) => (b.power > a.power ? b : a));
const fine = bls(
    Array.from({ length: 201 }, (_, i) => top.P - 0.01 + i * 0.0001),
    [0.1, 0.11, 0.12, 0.13],
).reduce((a, b) => (b.power > a.power ? b : a));
const powers = new DataFrame({ p: Float64Array.from(periodogram.map((r) => r.power)) });
const sde = (top.power - powers.MEAN("p")) / powers.STDDEV("p");
const rStar = 0.92,
    rPlanetEarth = Math.sqrt(fine.depth) * rStar * 109.1;

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
results["Julian Day of Sputnik's launch matches Meeus example 7.a (2436116.31)"] =
    Math.abs(julianDay(1957, 10, 4.81) - 2436116.31) < 1e-6;
const g = gmst(julianDay(1987, 4, 10)) / 15,
    gh = Math.floor(g),
    gm = Math.floor((g - gh) * 60),
    gs = ((g - gh) * 60 - gm) * 60;
results[`GMST 1987 April 10 0h UT = ${gh}h${gm}m${gs.toFixed(4)}s (Meeus example 12.a: 13h10m46.3668s)`] =
    gh === 13 && gm === 10 && Math.abs(gs - 46.3668) < 0.01;
const earthR = Math.hypot(...helio("Earth", jdNight)),
    marsR = Math.hypot(...helio("Mars", jdNight));
results[`heliocentric distances are physical (Earth ${earthR.toFixed(4)} AU, Mars ${marsR.toFixed(3)} AU)`] =
    earthR > 0.98 && earthR < 1.02 && marsR > 1.38 && marsR < 1.67;
const venus = planets.find((p) => p.name === "Venus");
results[`Venus never strays more than 47° from the Sun (elongation ${venus.elong.toFixed(1)}°)`] = venus.elong < 47.5;
results[`astronomical darkness lasts ${((darkEnd - darkStart) * 24).toFixed(1)} h`] =
    darkStart && darkEnd && darkEnd > darkStart && (darkEnd - darkStart) * 24 > 7 && (darkEnd - darkStart) * 24 < 11;
results["no object rises higher than its culmination altitude 90° − |latitude − declination|"] = plan.every(
    (o) => o.best.alt <= 90 - Math.abs(SITE.lat - o.dec) + 0.5,
);
results["the planner only recommends objects above 30° in darkness"] =
    plan[0].score > 0 && plan.filter((o) => o.score > 0).every((o) => o.best.alt >= 30);
results[
    `the KDTree finds the objects within ${MOON_GLARE}° of the Moon exactly as a separation scan does (${nearMoon.join(" ") || "none"})`
] =
    nearMoon.join() ===
    catalog
        .filter((o) => separation(o, moonMid) <= MOON_GLARE)
        .map((o) => o.id)
        .sortBy((x) => x)
        .join();
results[
    `BLS recovers P = ${fine.P.toFixed(4)} d (true ${TRUE.period}), depth ${(fine.depth * 1e6).toFixed(0)} ppm (true ${TRUE.depth * 1e6}), SDE ${sde.toFixed(1)}`
] = Math.abs(fine.P - TRUE.period) < 0.002 && Math.abs(fine.depth / TRUE.depth - 1) < 0.2 && sde > 7;

const hhmm = (jd) => (jd ? formatRFC3339(Math.floor((jd - 2440587.5) * 86400)).slice(11, 16) : "—");
print("astronomy");
print(`  moon glare within ${MOON_GLARE}°: ${nearMoon.join(", ") || "none"}`);
print(
    `  site       ${SITE.name} (${SITE.lat}°, ${SITE.lon}°); darkness ${hhmm(darkStart)}–${hhmm(darkEnd)} UT; Moon ${(illum * 100).toFixed(0)}% illuminated`,
);
print(
    table([
        ["planet", "RA", "Dec", "dist AU", "elongation", "rise", "transit", "set"],
        ...planets.map((p) => [
            p.name,
            `${(p.ra / 15).toFixed(2)}h`,
            `${p.dec.toFixed(1)}°`,
            p.dist.toFixed(3),
            `${p.elong.toFixed(1)}°`,
            hhmm(p.rs.rise),
            hhmm(p.rs.transit),
            hhmm(p.rs.set),
        ]),
    ]),
);
print(
    table([
        ["object", "type", "mag", "best UT", "alt", "airmass", "moon sep", "score"],
        ...plan
            .slice(0, 8)
            .map((o) => [
                `${o.id} ${o.name}`,
                o.type,
                String(o.mag),
                hhmm(o.best.jd),
                `${o.best.alt.toFixed(0)}°`,
                Number.isFinite(o.airmass) ? o.airmass.toFixed(2) : "—",
                `${o.moonSep.toFixed(0)}°`,
                o.score.toFixed(2),
            ]),
    ]),
);
print(
    `  transit    ${t.length} points over ${DAYS} d; BLS best period ${fine.P.toFixed(4)} d, duration ${(fine.dur * 24).toFixed(1)} h, depth ${(fine.depth * 1e6).toFixed(0)} ppm, SDE ${sde.toFixed(1)} -> R ≈ ${rPlanetEarth.toFixed(1)} R⊕ for a ${rStar} R☉ star`,
);
print(
    `  checksum   ${sum(Float32Array.from(detrended.slice(0, 100))).toFixed(3)} (sum of the first 100 detrended points)`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
