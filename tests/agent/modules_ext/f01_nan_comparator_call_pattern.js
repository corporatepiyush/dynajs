__EXP = {};
__EXP[1] = [["nancmp: 3,NaN,1,NaN,2 calls=4", "nancmp: 3,NaN,1,NaN,2 calls=5"]];
__EXP[2] = ["f32: NaN,7,NaN,-1 calls=0"];
__EXP[3] = ["DONE"];
__REQ = {"dynajs": {"1": 1, "2": 1, "3": 1}, "node": {"1": 1, "2": 1, "3": 1}};
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const f = new Float64Array([3, NaN, 1, NaN, 2]);
let calls = 0;
f.sort(function (a, b) { calls++; return NaN; });
__L(1, 'nancmp: ' + f.join(',') + ' calls=' + calls);
const f2 = new Float32Array([NaN, 7, NaN, -1]);
calls = 0;
f2.sort(() => NaN);
__L(2, 'f32: ' + f2.join(',') + ' calls=' + calls);
__L(3, 'DONE');

summary("modules_ext");
