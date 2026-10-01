import { fff1 } from "./fff1.so";

var N = 0, BAD = 0;
function chk(name, cond) { N++; if (!cond) { BAD++; console.log("FAIL", name); } }

var mod = { f: fff1 };

function callLoc(m, x) {
    var r = m.f(x);
    return r;
}
chk("fff1-loc-nan", isNaN(callLoc(mod, 3)));

function callImm(m) {
    var r = m.f(2);
    return r;
}
chk("fff1-imm-nan", isNaN(callImm(mod)));

var nanCount = 0;
for (var i = 0; i < 500; i++) {
    var v = callLoc(mod, i);
    if (isNaN(v)) nanCount++;
}
chk("fff1-loop", nanCount === 500);

function callTwo(m, a, b) {
    return m.ff(a, b);
}
var mod2 = { ff: fff1 };
chk("fff1-two-args", callTwo(mod2, 2, 3) === 5);

console.log("rv04_native_fff1 probes=" + N + " failed=" + BAD);
