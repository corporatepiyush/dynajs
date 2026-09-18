function tG(n) { if (n === 0) return typeof this; return tG(n - 1); }
console.log("ss4a", tG(3));
console.log("ss4b-start");
console.log("ss4b", tG(200000));
