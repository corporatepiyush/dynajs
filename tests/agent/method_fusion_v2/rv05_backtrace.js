// rv05_backtrace — pinned LINE-level source-attribution fidelity for fused
// method calls. The fold keeps the interior ops' pc2line offsets, so every
// stage of a fused call that surfaces in a stack trace reports the SAME LINE
// the unfused engine reports.
//
// COLUMN-level attribution intentionally differs at fused call sites (the
// interior ops' columns are no longer resolvable; deterministic per site, e.g.
// 51:39 fused vs 51:41 unfused). This probe therefore pins LINE numbers only,
// SELF-RELATIVELY (each asserted line is an offset from an anchor Error
// captured at a known statement), so the file can be edited without breaking
// the pins.
//
// Engine stack shape (pre-existing, IDENTICAL on pristine / gate=0 / fused —
// verified): a propagated callee throw emits [callee throw line, handler
// line] and omits intermediate JS call-site frames; exceptions raised AT the
// call (not-a-function) and getter throws during the load DO carry the fused
// call-site line. Node inserts the callee's frame — node-side difference.
//
// Gate: outputs must be byte-identical across the ON (fused), OFF (gate=0)
// and pristine binaries.
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

// 1. callee throw through a fused method1_loc call: frame 0 is the callee's
//    throw line (same physical line as the callee definition, offset 0).
var callee1Anchor = lineOf(new Error().stack, 0);
var thrower = { m: function () { throw new RangeError("boom"); } };
function f1(o, v) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m(v);
}
var l1 = -1;
try { f1(thrower, 1); } catch (e) { l1 = lineOf(e.stack, 0); }
chk("callee-throw-line", l1 === callee1Anchor + 1);

// 2. not-a-function TypeError raised AT the fused call: frame 0 is the fused
//    call line (caller anchor + 1 — the call is the line after the anchor).
function f2(o) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m2();
}
var l2 = -1;
try { f2({ m2: 42 }); } catch (e) { l2 = lineOf(e.stack, 0); }
chk("notafunc-is-call-line", l2 === gAnchor + 1);

// 3. fused method1_imm8 (immediate arg), callee throw: callee line again.
var callee3Anchor = lineOf(new Error().stack, 0);
var immThrower = { charCodeAt: function () { throw new TypeError("x"); } };
function f3(s) {
    gAnchor = lineOf(new Error().stack, 0);
    return s.charCodeAt(100);
}
var l3 = -1;
try { f3(immThrower); } catch (e) { l3 = lineOf(e.stack, 0); }
chk("imm8-throw-line", l3 === callee3Anchor + 1);

// 4. fused method0 (no argument), callee throw.
var callee4Anchor = lineOf(new Error().stack, 0);
var zeroThrower = { m4: function () { throw new RangeError("y"); } };
function f4(o) {
    gAnchor = lineOf(new Error().stack, 0);
    return o.m4();
}
var l4 = -1;
try { f4(zeroThrower); } catch (e) { l4 = lineOf(e.stack, 0); }
chk("method0-throw-line", l4 === callee4Anchor + 1);

// 5. two fused calls on one source line, the first raises not-a-function
//    (receiver has no m5): frame 0 is THAT shared source line.
function f5(o) {
    gAnchor = lineOf(new Error().stack, 0);
    o.m5(1); o.m5(2);
    return 0;
}
var l5 = -1;
try { f5({}); } catch (e) { l5 = lineOf(e.stack, 0); }
chk("same-line-second-call", l5 === gAnchor + 1);

// 6. getter throwing during the fused LOAD: the getter's throw sits on the
//    same line as its accessor (anchor + 3) and the next frame is the fused
//    call line (f6's anchor + 1).
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
    l6a = lineOf(e.stack, 0); /* getter's throw line */
    l6b = lineOf(e.stack, 1); /* the fused call line */
}
chk("getter-throw-line", l6a === getterAnchor + 3);
chk("getter-caller-is-call-line", l6b === gAnchor + 1);

// 7. sanity.
chk("anchors-positive", gAnchor > 0 && getterAnchor > 0 && callee1Anchor > 0);

console.log("rv05_backtrace probes=" + N + " failed=" + BAD);
