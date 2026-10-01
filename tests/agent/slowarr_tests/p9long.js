let a = [];
for (let i = 0; i < 1000; i++) a.push(i);
delete a[500];
let t0 = performance.now();
for (let i = 0; i < 400000; i++) a.reverse();
console.log((performance.now() - t0).toFixed(1));
