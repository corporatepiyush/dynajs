var N = 0, BAD = 0;
function chk(name, cond) { N++; if (!cond) { BAD++; console.log("FAIL", name); } }

function lineOf(stackStr, frame) {
    var lines = stackStr.split("\n");
    for (var i = 0; i < lines.length; i++) {
        var m = lines[i].match(/rv05_backtrace\.js:(\d+):/);
        if (m) {
            if (frame === 0) return parseInt(m[1], 10);
            frame--;
        }
    }
    return -1;
}

var callee1Anchor = lineOf(new Error().stack, 0);
var thrower = { m: function () { throw new RangeError("boom"); } };
function f1(o, v) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m(v);
}
var l1 = -1;
try { f1(thrower, 1); } catch (e) { l1 = lineOf(e.stack, 0); }
chk("callee-throw-line", l1 === callee1Anchor + 1);

function f2(o) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m2();
}
var l2 = -1;
try { f2({ m2: 42 }); } catch (e) { l2 = lineOf(e.stack, 0); }
chk("notafunc-is-call-line", l2 === gAnchor + 1);

var callee3Anchor = lineOf(new Error().stack, 0);
var immThrower = { charCodeAt: function () { throw new TypeError("x"); } };
function f3(s) {
    gAnchor = lineOf(new Error().stack, 0);
    return s.charCodeAt(100);
}
var l3 = -1;
try { f3(immThrower); } catch (e) { l3 = lineOf(e.stack, 0); }
chk("imm8-throw-line", l3 === callee3Anchor + 1);

var callee4Anchor = lineOf(new Error().stack, 0);
var zeroThrower = { m4: function () { throw new RangeError("y"); } };
function f4(o) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m4();
}
var l4 = -1;
try { f4(zeroThrower); } catch (e) { l4 = lineOf(e.stack, 0); }
chk("method0-throw-line", l4 === callee4Anchor + 1);

function f5(o) {
    gAnchor = lineOf(new Error().stack, 0);
    o.m5(1); o.m5(2);
    return 0;
}
var l5 = -1;
try { f5({}); } catch (e) { l5 = lineOf(e.stack, 0); }
chk("same-line-second-call", l5 === gAnchor + 1);

var getterAnchor = lineOf(new Error().stack, 0);
var g = {};
Object.defineProperty(g, "m6", {
    get: function () { throw new TypeError("getter"); },
    configurable: true
});
function f6(o) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m6();
}
var l6a = -1, l6b = -1;
try { f6(g); } catch (e) {
    l6a = lineOf(e.stack, 0);
    l6b = lineOf(e.stack, 1);
}
chk("getter-throw-line", l6a === getterAnchor + 3);
chk("getter-caller-is-call-line", l6b === gAnchor + 1);

chk("anchors-positive", gAnchor > 0 && getterAnchor > 0 && callee1Anchor > 0);

console.log("rv05_backtrace probes=" + N + " failed=" + BAD);
