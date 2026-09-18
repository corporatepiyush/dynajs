// holey-1k: one hole mid-array
let b = [];
for (let i = 0; i < 1000; i++) b.push(i);
delete b[500];
b.unshift(9);
b.shift();
