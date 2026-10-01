var N = 0, BAD = 0;
function chk(name, cond) { N++; if (!cond) { BAD++; console.log("FAIL", name); } }

var M = Math;

function absLoc(x)  { var r = M.abs(x);  return r; }
function floorLoc(x){ var r = M.floor(x); return r; }
function ceilLoc(x) { var r = M.ceil(x);  return r; }
function roundLoc(x){ var r = M.round(x); return r; }
function sqrtLoc(x) { var r = M.sqrt(x);  return r; }
function expLoc(x)  { var r = M.exp(x);   return r; }

chk("abs-loc", absLoc(-4.2) === 4.2);
chk("floor-loc", floorLoc(3.9) === 3);
chk("ceil-loc", ceilLoc(3.1) === 4);
chk("round-loc", roundLoc(3.5) === 4);
chk("sqrt-loc", sqrtLoc(9) === 3);
chk("exp-loc", Math.round(expLoc(2) * 1e6) === 7389056);

function absImm()  { return M.abs(-3); }
function sqrtImm() { return M.sqrt(4); }
chk("abs-imm8", absImm() === 3);
chk("sqrt-imm8", sqrtImm() === 2);

var s = 0;
for (var i = 1; i <= 500; i++) s += M.sqrt(i);
chk("sqrt-loop", Math.round(s * 1000) === 7464534);

function sqrt0() { return M.sqrt(); }
chk("sqrt-argc0", isNaN(sqrt0()));

chk("sqrt-nan-arg", isNaN(M.sqrt(-1)));
chk("abs-nan-arg", isNaN(M.abs(undefined)));

var holder = {};
Object.defineProperty(holder, "m", {
    get: function () { return M.sqrt; },
    configurable: true
});
function getterSqrt(o, v) { return o.m(v); }
chk("getter-native", getterSqrt(holder, 16) === 4);

var protoHost = Object.create(M);
function protoSqrt(o, v) { return o.sqrt(v); }
chk("proto-native", protoSqrt(protoHost, 25) === 5);

var eater = { m: function (x) { x = 1; return x; } };
function eat(o, v) { return o.m(v); }
chk("eat-param", eat(eater, 41) === 1);

console.log("rv04_native probes=" + N + " failed=" + BAD);
