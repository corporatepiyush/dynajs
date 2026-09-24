// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["f64-identity: 9,8,7,6,5,4,3,2,1,0"];
__EXP[2] = ["eq: 1,1,1"];
__EXP[3] = ["i8-identity: 3,2,1"];
__EXP[4] = ["nan-always0: 1,NaN,2,NaN"];
__EXP[5] = ["DONE"];
// modules_ext f09: comparator always returns 0 -> permutation must be IDENTITY
// (stability by offset for both the rqsort-with-idx path and NaN-pair path).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const f = new Float64Array([9, 8, 7, 6, 5, 4, 3, 2, 1, 0]);
f.sort(() => 0);
__L(1, 'f64-identity: ' + f.join(','));
const g = new Float64Array([1, 1, 1]);
g.sort(() => 0);
__L(2, 'eq: ' + g.join(','));
const i = new Int8Array([3, 2, 1]);
i.sort(() => 0);
__L(3, 'i8-identity: ' + i.join(','));
// NaN + always-0 comparator: NaN pairs compare equal (0), but dynajs forces NaN last
const n = new Float64Array([1, NaN, 2, NaN]);
n.sort(() => 0);
__L(4, 'nan-always0: ' + n.join(','));
__L(5, 'DONE');

summary("modules_ext");
