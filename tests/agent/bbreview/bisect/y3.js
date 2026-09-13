const arrow = (n) => n === 0 ? "arr-done" : arrow(n - 1);
console.log("ss5", arrow(5));
console.log("ss6-start");
try { console.log("ss6", arrow(300000)); } catch (e) { console.log("ss6-overflow", e.constructor.name); }
