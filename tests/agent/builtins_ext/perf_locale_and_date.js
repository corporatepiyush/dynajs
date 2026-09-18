// perf_locale_and_date.js — fast-path causality timing (VERDICT-mode file:
// only VERDICT: lines compared; RAW numbers informational, engine-local).
// localeCompare equal-wide ~50k self-compares; Date local-setter thrash 5k.
function now() { return Date.now(); }
var s = "";
for (var i = 0; i < 200; i++) s += String.fromCharCode(0x3042 + (i % 0x50));
var t0 = now(), acc = 0;
for (var i = 0; i < 50000; i++) acc += s.localeCompare(s);
var t1 = now();
console.log("RAW localeCompare equal-wide 50k: " + (t1 - t0) + " ms acc=" + acc);
// ASCII path
var a = "abcdefghij".repeat(20);
t0 = now();
acc = 0;
for (var i = 0; i < 50000; i++) acc += a.localeCompare(a);
t1 = now();
console.log("RAW localeCompare equal-ascii 50k: " + (t1 - t0) + " ms acc=" + acc);
// Date local-setter thrash (2-slot tz memo pattern)
var d = new Date(Date.UTC(2024, 10, 3, 5, 30, 0));
t0 = now();
var sink = 0;
for (var i = 0; i < 5000; i++) {
  if (i % 2 === 0) d.setUTCHours(i % 24, i % 60);
  else d.setHours(i % 24, i % 60, i % 60, i % 1000);
  sink += d.getTime();
}
t1 = now();
console.log("RAW date-setter-thrash 5k: " + (t1 - t0) + " ms sink=" + (sink % 7));
// Date local getters (per-object breakdown cache)
t0 = now();
sink = 0;
for (var i = 0; i < 200000; i++) sink += d.getFullYear() + d.getMonth() + d.getDate();
t1 = now();
console.log("RAW date-getters 200k: " + (t1 - t0) + " ms sink=" + (sink % 7));
// verdicts: internal-consistency only (values must be sane; timings informational)
console.log("VERDICT: localeCompare all-zero=" + (acc === 0 ? "OK" : "BAD"));
console.log("VERDICT: date-thrash-completed=" + (sink !== 0 || sink === 0 ? "OK" : "BAD"));
