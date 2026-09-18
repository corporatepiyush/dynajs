const out = typeof console !== "undefined" ? console.log : print;
const a = [1, 2];
let n = 0;
Object.defineProperty(a, Symbol.iterator, {
  get() { n++; return (n === 1) ? function* () { yield "A"; } : function* () { yield "B"; }; },
  configurable: true,
});
out(JSON.stringify([...a]));
out("gets=" + n);
