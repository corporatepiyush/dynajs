const w1 = "\u00e9\u4e2d\u6587".repeat(70).slice(0, 200);
const w2 = "\u00e9\u4e2d\u6587".repeat(70).slice(0, 200);
let t0 = Date.now();
let sink = 0;
for (let i = 0; i < 100000; i++) sink += w1.localeCompare(w2);
console.log("equal-200wide:", (Date.now() - t0) / 100, "us/call sink=" + sink);
