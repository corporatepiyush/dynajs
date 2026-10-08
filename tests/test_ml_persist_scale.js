// timeout: 180
// R5-3: the ML persistence path shares the DYNS decoder; a legitimately
// trained model whose record is well over the old 64x-payload guard's reach
// (a >64 MiB numeric payload) must still load. MinMaxScaler stores two
// columns of doubles, so cols = 3.3M encodes to roughly 76 MiB.
import * as ml from "dyna:ml";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

const cols = 3300000;
const X = new Float64Array(2 * cols);
for (let i = 0; i < cols; i++) {
    X[i] = (i % 97) - 48;
    X[cols + i] = X[i] + 10;
}

const src = new ml.MinMaxScaler();
src.fit(X, 2, cols);
const bytes = src.serialize();
check(bytes instanceof Uint8Array, "serialize returns bytes");
check(bytes.length > (64 << 20),
    "the model record is over 64 MiB (got " + (bytes.length >>> 20) + " MiB)");

let back = null;
try {
    back = ml.MinMaxScaler.deserialize(bytes);
} catch (e) {
    fails++;
    print("FAIL: a legitimate >64 MiB ML record must deserialize: " + e);
}
if (back) {
    check(back.dataMin.length === cols && back.dataMax.length === cols,
        "rotund model arrays survive (got " + back.dataMin.length + "/" + back.dataMax.length + ")");
    check(back.dataMin[0] === -48 && back.dataMax[0] === -38,
        "spot values survive the round trip (got " + back.dataMin[0] + "/" + back.dataMax[0] + ")");
    const lo = Float64Array.from(back.dataMin);
    const hi = new Float64Array(cols);
    for (let i = 0; i < cols; i++) hi[i] = lo[i] + 10;
    const loOut = back.transform(lo, 1, cols);
    const hiOut = back.transform(hi, 1, cols);
    check(loOut[0] === 0 && loOut[cols - 1] === 0 && hiOut[0] === 1 && hiOut[cols - 1] === 1,
        "the loaded model transforms exactly (got " + loOut[0] + "," + hiOut[cols - 1] + ")");
}

if (fails === 0) print("test_ml_persist_scale: all " + n + " checks passed");
else {
    print("test_ml_persist_scale: " + fails + " FAILED of " + n);
    throw new Error("test_ml_persist_scale: " + fails + " failures");
}
