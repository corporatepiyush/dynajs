function kind(a) { return Object.getOwnPropertyNames(a).length; }
let a = [];
for (let i = 0; i < 1000; i++) a.push(i);
console.log("fast:", kind(a));
delete a[500];
console.log("after delete (999=slow,1000=fast):", kind(a));
let b = [];
b[1000000] = 1;
console.log("sparse1e6 own props:", kind(b), "len:", b.length);
let t0 = performance.now();
for (let i = 0; i < 1000; i++) b.indexOf(42);
console.log("indexOf sparse1e6 us/op:", (performance.now()-t0)*1000/1000);
let c = [];
for (let i = 0; i < 1000; i++) c.push(i);
delete c[500];
t0 = performance.now();
for (let i = 0; i < 1000; i++) c.indexOf(4242);
console.log("indexOf holey1k us/op:", (performance.now()-t0)*1000/1000);
