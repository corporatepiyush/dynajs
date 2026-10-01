var TARGET_MS = 60;
var sink = 0;

function measure(fn) {
    var reps = 1, ms = 0;
    for (;;) {
        var t0 = performance.now();
        for (var i = 0; i < reps; i++) sink = fn(i);
        ms = performance.now() - t0;
        if (ms >= TARGET_MS || reps >= (1 << 22)) break;
        reps = Math.max(reps * 2, Math.ceil(reps * (TARGET_MS / Math.max(ms, 0.01))));
    }
    var best = ms / reps;
    for (var k = 0; k < 3; k++) {
        t0 = performance.now();
        for (var j = 0; j < reps; j++) sink = fn(j);
        var per = (performance.now() - t0) / reps;
        if (per < best) best = per;
    }
    return best * 1e6;
}

var ccStr = "";
for (var i = 0; i < 128; i++) ccStr += String.fromCharCode(32 + (i % 95));
function k_cc(n) {
    var h = 0;
    for (var i = 0; i < n; i++) h += ccStr.charCodeAt(i);
    return h;
}

var objLoc = { m: function (x) { return x + 1; } };
function k_mloc(n) {
    var o = objLoc, s = 0;
    for (var i = 0; i < n; i++) s += o.m(i);
    return s;
}

var objArg = { m: function (x) { return x + 2; } };
function k_marg(n) {
    var o = objArg, s = 0;
    for (var i = 0; i < n; i++) s += o.m(i);
    return s;
}

function k_sqrt(n) {
    var a = 0;
    for (var i = 1; i <= n; i++) a += Math.sqrt(i);
    return a;
}

function k_empty(n) {
    var s = 0;
    for (var i = 0; i < n; i++) s += 1;
    return s;
}

var obj2 = { m: function (a, b) { return a + b; } };
function k_call2(n) {
    var o = obj2, s = 0;
    for (var i = 0; i < n; i++) s += o.m(i, 3);
    return s;
}

var jsonish = '{"a":[1,2,{"b":"qu\\"ote"}],"c":{"d":[true,null,"x\\n"],"e":[1.5,-2,{}]},"f":"tail"}';
while (jsonish.length < 2048) jsonish += jsonish;
function k_scan(passes) {
    var src = jsonish, total = 0;
    for (var p = 0; p < passes; p++) {
        var depth = 0, inStr = 0, esc = 0;
        for (var i = 0, len = src.length; i < len; i++) {
            var c = src.charCodeAt(i);
            if (inStr) {
                if (esc) esc = 0;
                else if (c === 92) esc = 1;
                else if (c === 34) inStr = 0;
            } else {
                if (c === 34) inStr = 1;
                else if (c === 123 || c === 91) depth++;
                else if (c === 125 || c === 93) depth--;
            }
        }
        total += depth;
    }
    return total;
}

function k_churn(n) {
    var nums = [];
    for (var i = 0; i < n; i++) nums.push(((i * 2654435761) % 997).toFixed(1));
    var acc = "";
    for (var j = 0; j < nums.length; j++) acc += nums[j].charAt(nums[j].length - 1);
    var sum = 0;
    for (var k = 0; k < acc.length; k++) sum += acc.charCodeAt(k);
    return sum;
}

var checks = 0;
checks += k_cc(128);
checks += k_mloc(125000) % 7;
checks += k_marg(500000) % 11;
checks += (k_sqrt(10000) * 1000) % 13;
checks += k_empty(100000) % 17;
checks += k_call2(125000) % 19;
checks += k_scan(4) % 23;
checks += k_churn(400) % 29;
if (checks !== checks) throw new Error("NaN in kernels");

console.log("kernel\tns");
console.log("cc_loop\t" + measure(function () { return k_cc(128); }).toFixed(1));
console.log("method_loc\t" + measure(function () { return k_mloc(125000); }).toFixed(1));
console.log("method_arg\t" + measure(function () { return k_marg(500000); }).toFixed(1));
console.log("sqrt_loop\t" + measure(function () { return k_sqrt(10000); }).toFixed(1));
console.log("empty_loop\t" + measure(function () { return k_empty(100000); }).toFixed(1));
console.log("call2_ctrl\t" + measure(function () { return k_call2(125000); }).toFixed(1));
console.log("task_scan\t" + measure(function () { return k_scan(4); }).toFixed(1));
console.log("task_churn\t" + measure(function () { return k_churn(400); }).toFixed(1));
console.log("sink\t" + (sink !== undefined ? 1 : 0));
