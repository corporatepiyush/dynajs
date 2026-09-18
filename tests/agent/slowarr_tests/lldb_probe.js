// LLDB probe: includes-miss 1e6-hole + unshift-hole-1k (audit cases)
let a = []; a[1000000] = 1;
for (let i = 0; i < 20; i++) a.includes(42);
for (let i = 0; i < 20; i++) a.indexOf(42);
let b = [];
for (let i = 0; i < 1000; i++) b.push(i);
delete b[500];
for (let i = 0; i < 50; i++) { b.unshift(9); b.shift(); }
console.log("probe done");
