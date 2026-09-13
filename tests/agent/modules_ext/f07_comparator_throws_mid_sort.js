// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["name=Boom marker=true calls=4"];
__EXP[2] = ["content=5,2,9,1,7"];
__EXP[3] = ["DONE"];
// modules_ext f07: comparator throws on the Nth call: exception propagates (original
// object identity), and the array content is whatever the partial sort left -- node
// byte-oracle on the leftover content and call count.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
class Boom extends Error { constructor() { super('b'); this.name = 'Boom'; } }
const f = new Float64Array([5, 2, 9, 1, 7]);
let calls = 0;
let caught = null;
try {
  f.sort(function (a, b) {
    calls++;
    if (calls === 4) throw new Boom();
    return a - b;
  });
} catch (e) { caught = e; }
__L(1, 'name=' + (caught && caught.name) + ' marker=' + (caught && caught instanceof Boom) + ' calls=' + calls);
__L(2, 'content=' + f.join(','));
__L(3, 'DONE');

summary("modules_ext");
