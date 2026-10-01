__EXP = {};
__EXP[0] = ["A [\"a0\",\"a1\",\"b0\",\"b1\"] | >a0,>a1,>a2,>b0,>b1,>b2"];
__EXP[1] = ["B [\"p\",\"a0\",\"q\",\"b0\"] | Vp,>a0,>a1,Vq,>b0,>b1"];
__EXP[2] = ["C [\"a0\",\"b0\"] | >a0,>a1,>b0,>b1"];
__EXP[3] = ["D [\"1\",\"a0\",\"a1\",\"2\",\"b0\"] | V1,>a0,>a1,>a2,V2,>b0,>b1"];
__EXP[4] = ["E [1,2,0,0,9] sinklen=5"];
const out = typeof console !== "undefined" ? console.log : print;
const log = [];
const src = (name, n) => ({
  [Symbol.iterator]() {
    let i = 0;
    return { next: () => log.push(">" + name + i) && (i < n ? { value: name + i++, done: false } : { done: true }) };
  },
});
const plain = (name) => { log.push("V" + name); return name; };

log.length = 0;
const r1 = [...src("a", 2), ...src("b", 2)];
__L(0, "A " + JSON.stringify(r1) + " | " + log.join(","));

log.length = 0;
const r2 = [plain("p"), ...src("a", 1), plain("q"), ...src("b", 1)];
__L(1, "B " + JSON.stringify(r2) + " | " + log.join(","));

log.length = 0;
const r3 = [...src("a", 1), ...[], ...src("b", 1)];
__L(2, "C " + JSON.stringify(r3) + " | " + log.join(","));

log.length = 0;
const f = (...args) => args;
log.length = 0;
const r4 = f(plain("1"), ...src("a", 2), plain("2"), ...src("b", 1));
__L(3, "D " + JSON.stringify(r4) + " | " + log.join(","));

{
  const sink = [];
  const peek = {
    [Symbol.iterator]() {
      let i = 0;
      return { next: () => ({ value: (i++, sink.length), done: i > 2 }) };
    },
  };
  const r5 = [ ...[1, 2], ...peek, 9 ];
  sink.push(r5.length);
  __L(4, "E " + JSON.stringify(r5) + " sinklen=" + sink[0]);
}

summary("arrays_ext");
