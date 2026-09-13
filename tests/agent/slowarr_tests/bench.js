// Perf bench: slow-array builtins. 3 reps, min. "name: us/op chk=" per inner op.
// Runs under dynajs and node.
"use strict";
function now() { return performance.now(); }

function makeHoley1k() {
    let a = [];
    for (let i = 0; i < 1000; i++) a.push(i);
    delete a[500];
    return a;
}
function makeHoley1kLate() {
    let a = [];
    for (let i = 0; i < 1000; i++) a.push(i);
    delete a[998];
    return a;
}
function makeSparse1e6() {
    let a = [];
    a[1000000] = 1;
    return a;
}
function makeSparse1e6few() {
    let a = [];
    for (let i = 0; i < 10; i++) a[i * 100000] = i;
    a.length = 1000001;
    return a;
}
function makeDenseSlow1k() { // dense but demoted: hole refilled -> occ == len, slow storage
    let a = [];
    for (let i = 0; i < 1000; i++) a.push(i);
    delete a[500];
    a[500] = 500;
    return a;
}
function makeDenseFast1k() {
    let a = [];
    for (let i = 0; i < 1000; i++) a.push(i);
    return a;
}

const cases = [];
function bench(name, n, inner, fn) { cases.push({ name, n, inner, fn }); }

// --- targets: slow-array paths ---
bench("P1 includes-miss sparse1e6", 20, 100, () => {
    let a = makeSparse1e6();
    let t0 = now(), r = 0;
    for (let i = 0; i < 100; i++) r += a.includes(42) ? 1 : 0;
    return [r, now() - t0];
});
bench("P2 indexOf-miss sparse1e6", 20, 100, () => {
    let a = makeSparse1e6();
    let t0 = now(), r = 0;
    for (let i = 0; i < 100; i++) r += a.indexOf(42) === -1 ? 1 : 0;
    return [r, now() - t0];
});
bench("P3 indexOf-miss holey1k", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.indexOf(4242) === -1 ? 1 : 0;
    return [r, now() - t0];
});
bench("P4 includes-miss holey1k", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.includes(4242) ? 1 : 0;
    return [r, now() - t0];
});
bench("P5 unshift+shift holey1k (per pair)", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) { a.unshift(9); r += a.shift(); }
    return [r, now() - t0];
});
bench("P6 unshift+shift holey1k-late-hole (per pair)", 50, 200, () => {
    let a = makeHoley1kLate();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) { a.unshift(9); r += a.shift(); }
    return [r, now() - t0];
});
bench("P7 unshift+shift denseSlow1k (per pair)", 50, 200, () => {
    let a = makeDenseSlow1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) { a.unshift(9); r += a.shift(); }
    return [r, now() - t0];
});
bench("P8 slice holey1k", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.slice().length;
    return [r, now() - t0];
});
bench("P9 reverse holey1k", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now();
    for (let i = 0; i < 200; i++) a.reverse();
    return [0, now() - t0];
});
bench("P10 concat holey1k", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.concat([1]).length;
    return [r, now() - t0];
});
bench("P11 lastIndexOf-miss holey1k", 50, 200, () => {
    let a = makeHoley1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.lastIndexOf(4242) === -1 ? 1 : 0;
    return [r, now() - t0];
});
bench("P12 shift sparse1e6 (per shift)", 20, 100, () => {
    let a = makeSparse1e6few();
    let t0 = now();
    for (let i = 0; i < 100; i++) { a[1000000] = 1; a.shift(); }
    return [a.length, now() - t0];
});
bench("P13 slice sparse1e6", 20, 100, () => {
    let a = makeSparse1e6();
    let t0 = now(), r = 0;
    for (let i = 0; i < 100; i++) r += a.slice().length;
    return [r, now() - t0];
});
bench("P14 includes-miss denseSlow1k", 50, 200, () => {
    let a = makeDenseSlow1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.includes(4242) ? 1 : 0;
    return [r, now() - t0];
});

// --- regression watch: fast paths must not slow down ---
bench("R1 includes dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.includes(999) ? 1 : 0;
    return [r, now() - t0];
});
bench("R2 indexOf dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.indexOf(4242) === -1 ? 1 : 0;
    return [r, now() - t0];
});
bench("R3 unshift+shift dense fast (per pair)", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) { a.unshift(9); r += a.shift(); }
    return [r, now() - t0];
});
bench("R4 slice dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.slice().length;
    return [r, now() - t0];
});
bench("R5 spread dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += [...a, 1].length;
    return [r, now() - t0];
});
bench("R6 for-of dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) for (const x of a) r += x;
    return [r, now() - t0];
});
bench("R7 reverse dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now();
    for (let i = 0; i < 200; i++) a.reverse();
    return [0, now() - t0];
});
bench("R8 concat dense fast", 50, 200, () => {
    let a = makeDenseFast1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) r += a.concat([1]).length;
    return [r, now() - t0];
});
bench("R9 push/pop dense slow", 50, 200, () => {
    let a = makeDenseSlow1k();
    let t0 = now(), r = 0;
    for (let i = 0; i < 200; i++) { a.push(i); r += a.pop(); }
    return [r, now() - t0];
});

const REPS = 3;
for (const c of cases) {
    let best = Infinity, chk = null;
    for (let r = 0; r < REPS; r++) {
        const [res, ms] = c.fn();
        chk = res;
        const us = ms * 1000 / c.inner; // one closure call = inner timed ops
        if (us < best) best = us;
    }
    console.log(c.name + ": " + best.toFixed(3) + " us/op  chk=" + chk);
}
