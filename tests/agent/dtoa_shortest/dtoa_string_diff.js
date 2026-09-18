// converted transcript generator: record stream pinned by
// count + FNV-1a digest + first-40 records (node oracle; dynajs divergences are explicit)
__PIN_EXP_N = 40;
__PIN_EXP_LINES = ["36dbbb70:e036180f|1.9430538399282263e-44", "863b2622:57f36f19|-1.1965125167134846e-278", "8b808da4:5f697cb3|-2.822266121744527e-253", "49f49a76:88ebcf5d|1.881991296298202e+48", "651c8918:20d97997|1.156334012133753e+179", "6dab4a0a:489bb1e1|1.9266284138029785e+220", "542fa1cc:4a3d72bb|3.3782796023817797e+97", "b18748de:45041aa5|-4.217164829535444e-70", "fa5f0bc0:f7b10c1f|-2.8177366863478806e+281", "8a47eaf2:db544da9|-3.8889682235201283e-259", "8c293af4:89d829c3|-4.404898653335144e-250", "7117c446:9dadceed|6.045406928362151e+236", "b3d9e368:6442efa7|-6.444134354177837e-59", "66a0a8da:f3286271|2.265226669245199e+186", "49bdf91c:11c1cb|1.7111605707593467e+47", "3d5facae:56160c35|4.501222173364241e-13", "9597b010:76d9442f|-1.1805021720414157e-204", "7f3923c2:f4871039|6.895973887867502e+304", "d527c44:e9c65ad3|1.692049272041295e-244", "24ca216:b70df27d|1.3681810265506305e-297", "c4f711b8:690229b7|-1.743060990556938e+24", "8108fbaa:f1c37701|-1.138466649728157e-303", "3df646c:3f5614db|5.033232620767676e-290", "c980447e:438aa1c5|-1.1608912767237615e+46", "b32aa860:c60fc03f|-3.240067191613779e-62", "57fbd092:6794b6c9|6.849726998306142e+115", "99715194:25250fe3|-3.980312259058565e-186", "70d033e6:fd253a0d|2.575877346457846e+235", "97b91408:a39827c7|-2.147138974912125e-194", "d71427a:8995ef91|6.319406863692972e-244", "5ba8e3bc:6c1b6beb|3.5333438748601967e+133", "30c6104e:389adb55|9.755943758206405e-74", "3afcf4b0:e9f5804f|1.4969810400126391e-24", "76bcf162:1ec64159|9.113775256581629e+263", "2c3abae4:222548f3|1.251414127971093e-95", "f31f79b6:24ca59d|-3.438651715915419e+246", "10a4ea58:acc5e9d7|1.7244070819083214e-228", "b3a67d4a:ee08cc21|-6.997601640178655e-60", "c36f770c:32b2c6fb|-70852948415363030", "794e101e:12bfb8e5|2.081698187403765e+276"];
__PIN_DYN_LINES = ["36dbbb70:e036180f|1.9430538399282263e-44", "863b2622:57f36f19|-1.1965125167134846e-278", "8b808da4:5f697cb3|-2.822266121744527e-253", "49f49a76:88ebcf5d|1.881991296298202e+48", "651c8918:20d97997|1.156334012133753e+179", "6dab4a0a:489bb1e1|1.9266284138029785e+220", "542fa1cc:4a3d72bb|3.3782796023817797e+97", "b18748de:45041aa5|-4.217164829535444e-70", "fa5f0bc0:f7b10c1f|-2.8177366863478806e+281", "8a47eaf2:db544da9|-3.8889682235201283e-259", "8c293af4:89d829c3|-4.404898653335144e-250", "7117c446:9dadceed|6.045406928362151e+236", "b3d9e368:6442efa7|-6.444134354177837e-59", "66a0a8da:f3286271|2.265226669245199e+186", "49bdf91c:11c1cb|1.7111605707593467e+47", "3d5facae:56160c35|4.501222173364241e-13", "9597b010:76d9442f|-1.1805021720414157e-204", "7f3923c2:f4871039|6.895973887867502e+304", "d527c44:e9c65ad3|1.692049272041295e-244", "24ca216:b70df27d|1.3681810265506305e-297", "c4f711b8:690229b7|-1.743060990556938e+24", "8108fbaa:f1c37701|-1.138466649728157e-303", "3df646c:3f5614db|5.033232620767676e-290", "c980447e:438aa1c5|-1.1608912767237615e+46", "b32aa860:c60fc03f|-3.240067191613779e-62", "57fbd092:6794b6c9|6.849726998306142e+115", "99715194:25250fe3|-3.980312259058565e-186", "70d033e6:fd253a0d|2.575877346457846e+235", "97b91408:a39827c7|-2.147138974912125e-194", "d71427a:8995ef91|6.319406863692972e-244", "5ba8e3bc:6c1b6beb|3.5333438748601967e+133", "30c6104e:389adb55|9.755943758206405e-74", "3afcf4b0:e9f5804f|1.4969810400126391e-24", "76bcf162:1ec64159|9.113775256581629e+263", "2c3abae4:222548f3|1.251414127971093e-95", "f31f79b6:24ca59d|-3.438651715915419e+246", "10a4ea58:acc5e9d7|1.7244070819083214e-228", "b3a67d4a:ee08cc21|-6.997601640178655e-60", "c36f770c:32b2c6fb|-70852948415363030", "794e101e:12bfb8e5|2.081698187403765e+276"];
// dtoa differential battery 1: String(d) corpus.
// Runs unmodified on dynajs AND node; prints one "hi16:lo16|String(d)" line
// (or "SPEC:hex" for NaN/Inf) per input. A driver diffs the outputs of two
// engines byte-for-byte; any difference is a dtoa regression.
// Covers: 100k deterministic random bit patterns (all exponent ranges),
// specials grid, every power of 10 +-1ulp, powers of 2 +-1ulp, the
// 1e20..1e22 boundary ladder, and 2^-1074 subnormal neighborhoods --
// the exact classes exercised by the Grisu-style shortest fast path in
// src/dtoa.c (radix-10 FREE format) and its exact-search fallback.
var out = (typeof print === "function") ? print : console.log;

var u32 = new Uint32Array(2);
var f64 = new Float64Array(u32.buffer);
function setbits(hi, lo) { u32[0] = lo; u32[1] = hi; }
// deterministic LCG for reproducibility
var s = 123456789 >>> 0;
function rnd() { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; }
var N = 100000;
for (var i = 0; i < N; i++) {
    var hi = rnd(), lo = rnd();
    setbits(hi, lo);
    var d = f64[0];
    if (d !== d || d === Infinity || d === -Infinity) { __PIN("SPEC:" + hi.toString(16) + lo.toString(16)); continue; }
    __PIN(hi.toString(16) + ":" + lo.toString(16) + "|" + String(d));
}
// specials grid
var specials = [0, -0, 1, -1, 0.5, 2, 1e-308, 5e-324, -5e-324, 1.7976931348623157e308,
    Number.MIN_VALUE, Number.MAX_VALUE, Number.EPSILON, 1e-7, 1e-6, 1e-5,
    1e20, 1e21, 1e22, 1e23, 9.999999999999999e21, 1.0000000000000002e21,
    4503599627370495, 4503599627370496, 9007199254740991, 9007199254740993,
    0.1, 0.2, 0.3, 1/3, 2/3, 1e-323, 2.5e-323, 1.5e-323];
// all powers of 10 +- 1 ulp
for (var k = -324; k <= 308; k++) {
    var p = Math.pow(10, k);
    if (p === 0) continue;
    specials.push(p);
    f64[0] = p; var h1 = u32[1], l1 = u32[0];
    setbits(h1, l1 + 1); if (f64[0] === f64[0] && f64[0] !== Infinity) specials.push(f64[0]);
    setbits(h1, l1 - 1); if (f64[0] === f64[0] && f64[0] !== -Infinity) specials.push(f64[0]);
}
// powers of two +-1 ulp across the whole range
for (var k2 = -1074; k2 <= 1023; k2 += 7) {
    var v = Math.pow(2, k2);
    if (v === Infinity || v !== v) continue;
    specials.push(v);
    f64[0] = v; var h2 = u32[1], l2 = u32[0];
    setbits(h2, l2 + 1); if (f64[0] === f64[0]) specials.push(f64[0]);
    setbits(h2, l2 - 1); if (f64[0] === f64[0]) specials.push(f64[0]);
}
// 1e20..1e22 fine steps
for (var m = 0; m < 2000; m++) {
    specials.push(1e20 + m * 1e16);
    specials.push(1e21 + m * 1e17);
}
// subnormal neighborhood
for (var q = 0; q < 2000; q++) {
    setbits(0, q); specials.push(f64[0]);
    setbits(0xFFFFF, 0xFFFFFFFF - q); specials.push(f64[0]);
}
for (var i2 = 0; i2 < specials.length; i2++) {
    var d2 = specials[i2];
    if (d2 !== d2 || d2 === Infinity || d2 === -Infinity) continue;
    __PIN(u32[1].toString(16) + ":" + u32[0].toString(16) + "|" + String(d2));
}

__A("transcript-record-count", function () { assert_eq(__PIN_count, 110830); });
__A("transcript-digest-fnv1a", function () { assert_diverge((__PIN_h1 >>> 0).toString(16), "dd37657f", "6a5d2dc3", "fnv1a"); });
summary("dtoa_shortest");
