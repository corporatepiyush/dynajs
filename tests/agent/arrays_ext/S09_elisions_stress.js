// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A [null,1]"];
__EXP[1] = ["B [null,1]"];
__EXP[2] = ["C [null,null,1,2,null]"];
__EXP[3] = ["D [null]"];
__EXP[4] = ["E [null]"];
__EXP[5] = ["F [null,null,null,null]"];
__EXP[6] = ["G [1,null,null,2]"];
__EXP[7] = ["H [null,1]"];
__EXP[8] = ["I [null,1]"];
__EXP[9] = ["J 2"];
__EXP[10] = ["K 2:1|2"];
__EXP[11] = ["L 2:1|2"];
__EXP[12] = ["M 1:3"];
__EXP[13] = ["N 2:1|2"];
__EXP[14] = ["O [0,1,2,3]"];
__EXP[15] = ["P [9,0,1,2,3]"];
__EXP[16] = ["Q self [1,2,1,2]"];
__EXP[17] = ["R self2 [1,2,1,2,1,2]"];
__EXP[18] = ["S triple [7,8,7,8,7,8]"];
__EXP[19] = ["T [1,2,3] [1,2,3]"];
__EXP[20] = ["U 40 0 39"];
__EXP[21] = ["V [40]"];
__EXP[22] = ["W [, ...s] => len=3 [null,9,8]", "W [...s, ,] => len=3 [9,8,null]", "W [, ...s, ,] => len=4 [null,9,8,null]", "W [, , ...s] => len=4 [null,null,9,8]", "W [...s, , ,] => len=4 [9,8,null,null]", "W [, ...s, ...s, ,] => len=6 [null,9,8,9,8,null]", "W [, ...[], , ...s, ,] => len=5 [null,null,9,8,null]"];
// S09: elision x spread x EMPTY-spread stress — the exact count-over-write memory-corruption shape,
// now also inside calls (push(...)) and nested literals, with fast-path pressure beforehand.
const out = typeof console !== "undefined" ? console.log : print;
const id = (x) => x;

// pressure: build/destroy many arrays so a fresh fast array is reused from free lists
for (let i = 0; i < 200; i++) { const t = [i, i + 1, i + 2]; t.length = 0; t.push(i); }

__L(0, "A " + JSON.stringify([, ...[1]]));
__L(1, "B " + JSON.stringify([, ...[1], ]));
__L(2, "C " + JSON.stringify([, , ...[1, 2], , ]));
__L(3, "D " + JSON.stringify([...[], , ]));
__L(4, "E " + JSON.stringify([, ...[], ]));
__L(5, "F " + JSON.stringify([, , ...[], , ,]));
__L(6, "G " + JSON.stringify([...[1], , ...[], , ...[2]]));
__L(7, "H " + JSON.stringify([...[], ...[], , ...[1]]));

// deep nesting mixes
__L(8, "I " + JSON.stringify([...[[ , ...[1]]][0]]));
__L(9, "J " + JSON.stringify([[ , ...[1]], , ...[2]].flat ? [[ , ...[1]], , ...[2]].flat().length : "?"));

// same shapes as CALL arguments with elisions impossible — but spreads+empties in call position:
{
  const f = (...args) => args.length + ":" + args.join("|");
  __L(10, "K " + f(...[], ...[1, 2]));
  __L(11, "L " + f(...[1], ...[], ...[2]));
  __L(12, "M " + f(...[], ...[], ...[3]));
  __L(13, "N " + f(...[1, 2], ...[]));
}
// push bulk append with empty spreads interleaved
{
  const a = [0];
  a.push(...[], ...[1, 2], ...[], ...[3]);
  __L(14, "O " + JSON.stringify(a));
  a.unshift(...[], 9);
  __L(15, "P " + JSON.stringify(a));
}
// self-spread snapshot semantics (the other half of that regression): iterate+append same array
{
  const b = [1, 2];
  b.push(...b);
  __L(16, "Q self " + JSON.stringify(b));
  const c = [1, 2];
  c.push(...c, ...c);
  __L(17, "R self2 " + JSON.stringify(c));
  const d = [7, 8];
  const e2 = [...d, ...d, ...d];
  __L(18, "S triple " + JSON.stringify(e2));
}
// spread of an array literal that IS the destination under construction? (impossible in JS,
// but constructor-arg spread is close: new Array(...x) vs Array.of(...x))
{
  const x = [1, 2, 3];
  __L(19, "T " + JSON.stringify(new Array(...x)) + " " + JSON.stringify(Array.of(...x)));
}
// huge count of tiny spreads in one literal (cpool/register pressure)
{
  const many = [];
  const parts = [];
  for (let i = 0; i < 40; i++) parts.push(`...[${i}]`);
  const built = eval(`[${parts.join(", ")}]`);
  __L(20, "U " + built.length + " " + built[0] + " " + built[39]);
  many.push(built.length);
  __L(21, "V " + JSON.stringify(many));
}
// eval-shape guards keep the parser path honest: elisions generated dynamically
{
  const shapes = [
    "[, ...s]", "[...s, ,]", "[, ...s, ,]", "[, , ...s]",
    "[...s, , ,]", "[, ...s, ...s, ,]", "[, ...[], , ...s, ,]",
  ];
  for (const sh of shapes) {
    const r = eval(sh.replace(/s/g, "[9, 8]"));
    __L(22, "W " + sh + " => len=" + r.length + " " + JSON.stringify(r));
  }
}

summary("arrays_ext");
