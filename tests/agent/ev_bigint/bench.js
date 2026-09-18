// bench.js — bigint microbenchmarks. Usage: engine bench.js <mode> <digits> <iters> [radix]
//   modes: mul (digits x digits/2, unbalanced), mulbal (digits x digits), tostr (toString(radix))
// Operands built deterministically OUTSIDE the timed region.
// Sink is cheap (linear-time and-op) so it does not pollute the timing.
function buildBig(digits) {
  let state = 123456789123456789n;
  let parts = ["9"]; // leading nonzero -> exactly `digits` digits
  let rem = digits - 1;
  while (rem > 0) {
    state = (state * 1103515245n + 12345n) & 0x1fffffffffffffn;
    const d = state.toString();
    const take = rem < 19 ? rem : 19;
    parts.push(d.slice(0, take));
    rem -= take;
  }
  return BigInt(parts.join(""));
}
const mode = scriptArgs[1];
const digits = parseInt(scriptArgs[2]);
const iters = parseInt(scriptArgs[3]);
const radix = scriptArgs[4] ? parseInt(scriptArgs[4]) : 10;
let sink = 0;
const M = (a, b) => Number((a * b) & 0xffffffffffffffffn & 1n);
if (mode === "mul") {
  const a = buildBig(digits);
  const b = buildBig(digits >> 1 || digits); // second operand half size (or same if 1)
  const t0 = Date.now();
  for (let i = 0; i < iters; i++) { sink += M(a, b); }
  const t1 = Date.now();
  console.log("MUL " + digits + " " + iters + " " + (t1 - t0));
} else if (mode === "mulbal") {
  const a = buildBig(digits);
  const b = buildBig(digits); // balanced
  const t0 = Date.now();
  for (let i = 0; i < iters; i++) { sink += M(a, b); }
  const t1 = Date.now();
  console.log("MULBAL " + digits + " " + iters + " " + (t1 - t0));
} else if (mode === "tostr") {
  const v = buildBig(digits);
  const t0 = Date.now();
  for (let i = 0; i < iters; i++) { sink += v.toString(radix).length; }
  const t1 = Date.now();
  console.log("TOSTR " + digits + " r" + radix + " " + iters + " " + (t1 - t0));
} else if (mode === "add") {
  const a = buildBig(digits);
  const b = buildBig(digits); // balanced add; result fresh each iter
  const t0 = Date.now();
  for (let i = 0; i < iters; i++) { sink += Number((a + b) & 1n); }
  const t1 = Date.now();
  console.log("ADD " + digits + " " + iters + " " + (t1 - t0));
}
console.log("SINK " + (sink % 2));
