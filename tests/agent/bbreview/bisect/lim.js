function id() { return arguments.length; }
function tryN(n) { try { const a = new Array(n).fill(0); return id(...a) === n ? "ok" : "WRONG"; } catch (e) { return "ERR"; } }
let lo = 1, hi = 1;
while (tryN(hi) === "ok") { lo = hi; hi *= 2; if (hi > 4e6) break; }
while (lo < hi - 1) { const mid = (lo + hi) >> 1; if (tryN(mid) === "ok") lo = mid; else hi = mid; }
console.log("max-ok:", lo, "first-bad:", hi);
