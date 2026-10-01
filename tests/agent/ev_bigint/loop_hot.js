function buildBig(digits) {
  let state = 123456789123456789n;
  let parts = ["9"];
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
const a = buildBig(10000), b = buildBig(10000);
let sink = 0;
const ROUNDS = 30000;
for (let i = 0; i < ROUNDS; i++) {
  sink += Number((a * b) & 1n);
  sink += (a * b).toString().length;
}
console.log("DONE sink=" + (sink % 2));
