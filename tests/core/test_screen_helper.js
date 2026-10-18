// tests/lib/screen.js is the oracle for every pixel suite, so it is tested first: a whole-screen
// comparison that cannot fail would turn every graphics test green.
import { Frame, compareFrame } from "../lib/screen.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }

const W = 20, H = 12;
const img = (fill) => {
    const pixels = new Uint8Array(W * H * 4);
    for (let i = 0; i < W * H; i++) pixels.set(fill(i % W, (i / W) | 0), i * 4);
    return { width: W, height: H, pixels };
};
const BG = [10, 20, 30, 255], RED = [200, 0, 0, 255], BLUE = [0, 0, 250, 255];
const inRect = (x, y) => x >= 3 && x < 8 && y >= 2 && y < 6;

const want = new Frame(W, H, BG).rect(3, 2, 5, 4, RED);
ok(compareFrame(img((x, y) => (inRect(x, y) ? RED : BG)), want).ok, "a frame that matches everywhere passes");

let stray = 0;
for (const [sx, sy] of [[0, 0], [W - 1, H - 1], [8, 2], [3, 6], [11, 7]]) {
    const r = compareFrame(img((x, y) => (x === sx && y === sy ? BLUE : inRect(x, y) ? RED : BG)), want);
    if (!r.ok && r.mismatches === 1) stray++;
}
ok(stray === 5, "one stray pixel anywhere fails with exactly one mismatch (" + stray + "/5 caught)");
const holed = compareFrame(img((x, y) => (inRect(x, y) && !(x === 5 && y === 3) ? RED : BG)), want);
ok(!holed.ok && holed.mismatches === 1 && /\(5,3\)/.test(holed.report), "a hole inside a shape is reported at its coordinate");
ok(!compareFrame({ width: W - 1, height: H, pixels: new Uint8Array((W - 1) * H * 4) }, want).ok, "a frame of the wrong size fails");
ok(/X/.test(compareFrame(img(() => BLUE), want).report), "a failing report carries a mismatch map");

const text = new Frame(W, H, BG).oneOf(10, 4, 4, 3, [[255, 255, 255, 255]]);
ok(compareFrame(img((x, y) => (x >= 10 && x < 14 && y >= 4 && y < 7 && (x + y) % 2 ? [255, 255, 255, 255] : BG)), text).ok,
    "oneOf accepts the listed colour and the background under it");
ok(!compareFrame(img((x, y) => (x === 11 && y === 5 ? RED : BG)), text).ok, "oneOf refuses a colour it does not list");
ok(!compareFrame(img((x, y) => (x === 9 && y === 5 ? [255, 255, 255, 255] : BG)), text).ok, "and the listed colour outside the region");

const tri = new Frame(W, H, BG).triangle(2, 2, 18, 2, 2, 10, RED, 1);
const inside = (x, y) => x + 0.5 >= 2 && y + 0.5 >= 2 && (x + 0.5 - 2) / 16 + (y + 0.5 - 2) / 8 <= 1;
ok(compareFrame(img((x, y) => (inside(x, y) ? RED : BG)), tri).ok, "a triangle drawn by the centre rule passes the edge band");
ok(!compareFrame(img((x, y) => (x === 4 && y === 4 ? BG : inside(x, y) ? RED : BG)), tri).ok, "a missing pixel deep inside a triangle fails");
ok(!compareFrame(img((x, y) => (x === 15 && y === 9 ? RED : inside(x, y) ? RED : BG)), tri).ok, "a pixel far outside it fails");

const disc = new Frame(W, H, BG).circle(10, 6, 4, RED, 1);
const inDisc = (x, y) => Math.hypot(x + 0.5 - 10, y + 0.5 - 6) <= 4;
ok(compareFrame(img((x, y) => (inDisc(x, y) ? RED : BG)), disc).ok, "a disc drawn by the centre rule passes the circle's edge band");
ok(!compareFrame(img((x, y) => (x === 10 && y === 6 ? BG : inDisc(x, y) ? RED : BG)), disc).ok, "a hole at the disc's centre fails");
ok(!compareFrame(img((x, y) => (x === 1 && y === 1 ? RED : inDisc(x, y) ? RED : BG)), disc).ok, "a pixel far outside the disc fails");

const half = new Frame(W, H, BG).rect(2.5, 2.5, 3, 3, RED);
ok(half.want[2 * W + 2] !== half.want[0] && half.want[4 * W + 4] !== half.want[0] && half.want[5 * W + 5] === half.want[0] && half.want[W + 1] === half.want[0],
    "rect() owns a pixel when its centre is in [x, x + w): 2.5..5.5 fills pixels 2-4, not 1 or 5");

print("test_screen_helper: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_screen_helper: " + fail + " failed");
