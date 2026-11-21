// flags: --std
// Drives tools/android/demo.js on a running emulator or device through adb and checks what each
// step did, from the app's own log (build-apk.sh log) and screen captures. Run it through
// `tools/android/build-apk.sh smoke`, which builds and installs the demo first.
//
//   ./dynajs --std tools/android/smoke.js ADB      drive the device
//   ./dynajs --std tools/android/smoke.js selftest PNG decoder (all five filters, RGB and RGBA) and
//                                                  the screen-line parse, no device needed
//
// Coordinates come from the app's "screen" line (logical size, scale, insets), never from a
// guessed device: the nav bar is the bottom dock, 44 pt tall in the touch density, above the
// bottom inset; its three items share the width. Every check names what it expected.
import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";
import { inflate, deflate } from "dyna:compress";

const PKG = "org.dynajs.app", ACT = "org.dynajs.app/org.dynajs.app.DynaActivity";
let fails = 0;
function check(ok, what) {
    print((ok ? "ok    " : "FAIL  ") + what);
    if (!ok) fails++;
}

export function decodePng(d) {
    const be32 = (i) => ((d[i] << 24) | (d[i + 1] << 16) | (d[i + 2] << 8) | d[i + 3]) >>> 0;
    let i = 8, w = 0, h = 0, ct = 0;
    const idat = [];
    while (i + 8 <= d.length) {
        const n = be32(i);
        const t = String.fromCharCode(d[i + 4], d[i + 5], d[i + 6], d[i + 7]);
        const c = d.subarray(i + 8, i + 8 + n);
        i += 12 + n;
        if (t === "IHDR") { w = ((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]) >>> 0; h = ((c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7]) >>> 0; ct = c[9]; }
        else if (t === "IDAT") idat.push(c);
    }
    let len = 0;
    for (const c of idat) len += c.length;
    const z = new Uint8Array(len);
    let o = 0;
    for (const c of idat) { z.set(c, o); o += c.length; }
    const raw = inflate(z, { format: "zlib" });
    const bpp = ct === 6 ? 4 : 3, st = w * bpp;
    const out = new Uint8Array(st * h);
    let prev = new Uint8Array(st), p = 0;
    for (let y = 0; y < h; y++) {
        const f = raw[p];
        const line = raw.slice(p + 1, p + 1 + st);
        p += 1 + st;
        for (let x = 0; x < st; x++) {
            const a = x >= bpp ? line[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
            if (f === 1) line[x] = (line[x] + a) & 255;
            else if (f === 2) line[x] = (line[x] + b) & 255;
            else if (f === 3) line[x] = (line[x] + ((a + b) >> 1)) & 255;
            else if (f === 4) {
                const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
                line[x] = (line[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c)) & 255;
            }
        }
        out.set(line, y * st);
        prev = line;
    }
    return { w, h, bpp, px: out };
}

export const SCREEN_RE = /^screen ([\d.]+)x([\d.]+) pt (\d+)x(\d+) px scale ([\d.]+) (\w+) insets ([-\d.,]+)/;

function crc32(bytes) {
    let c = ~0 >>> 0;
    for (const b of bytes) {
        c ^= b;
        for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    }
    return ~c >>> 0;
}
function chunk(type, data) {
    const out = new Uint8Array(12 + data.length);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length);
    for (let k = 0; k < 4; k++) out[4 + k] = type.charCodeAt(k);
    out.set(data, 8);
    dv.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
    return out;
}
function encodePng(w, h, bpp, px, filters) {
    const st = w * bpp, raw = new Uint8Array((st + 1) * h);
    let prev = new Uint8Array(st);
    for (let y = 0; y < h; y++) {
        const f = filters[y % filters.length], line = px.subarray(y * st, y * st + st);
        raw[y * (st + 1)] = f;
        for (let x = 0; x < st; x++) {
            const a = x >= bpp ? line[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
            let pr = 0;
            if (f === 1) pr = a;
            else if (f === 2) pr = b;
            else if (f === 3) pr = (a + b) >> 1;
            else if (f === 4) { const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c); pr = pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
            raw[y * (st + 1) + 1 + x] = (line[x] - pr) & 255;
        }
        prev = line;
    }
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, w); dv.setUint32(4, h);
    ihdr[8] = 8; ihdr[9] = bpp === 4 ? 6 : 2;
    const z = deflate(raw, { format: "zlib" });
    const half = z.length >> 1;
    const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", z.subarray(0, half)), chunk("IDAT", z.subarray(half)), chunk("IEND", new Uint8Array(0))];
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}

function selftest() {
    let seed = 7;
    const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) >>> 24;
    for (const bpp of [3, 4]) {
        const w = 13, h = 10, px = new Uint8Array(w * h * bpp);
        for (let k = 0; k < px.length; k++) px[k] = rnd();
        const got = decodePng(encodePng(w, h, bpp, px, [0, 1, 2, 3, 4]));
        let same = got.w === w && got.h === h && got.bpp === bpp;
        for (let k = 0; same && k < px.length; k++) same = got.px[k] === px[k];
        check(same, `PNG ${bpp === 4 ? "RGBA" : "RGB"} ${w}x${h} over filters 0..4 and a split IDAT decodes to its pixels`);
    }
    const m = SCREEN_RE.exec("screen 411.4x914.3 pt 1080x2400 px scale 2.625 portrait insets 0,24,0,48");
    check(!!m && m[1] === "411.4" && m[4] === "2400" && m[7] === "0,24,0,48", "the screen line parses into size, pixels, scale and insets");
    check(SCREEN_RE.exec("screen ? pt") === null, "a malformed screen line does not parse");
    print(`android smoke selftest: ${fails} failure(s)`);
    std.exit(fails ? 1 : 0);
}

const arg = scriptArgs[1];
if (arg === "selftest") selftest();
if (!arg) { print("usage: smoke.js ADB | selftest"); std.exit(2); }
const ADB = arg;

const adb = (...a) => Exec(ADB, a, { encoding: "bytes", maxBuffer: 256 << 20 });
const adbText = (...a) => { const r = Exec(ADB, a); return typeof r.stdout === "string" ? r.stdout : ""; };
const log = () => adbText("shell", "run-as", PKG, "cat", "files/dynajs.log");
function last(kind) {
    const lines = log().split("\n").filter((l) => l.startsWith(kind + " "));
    return lines.length ? lines[lines.length - 1] : "";
}
function state() {
    const m = /^state (\{.*\}) frames/.exec(last("state"));
    if (!m) return {};
    try { return JSON.parse(m[1]); } catch (e) { return {}; }
}
const pause = (s) => os.sleep(Math.round(s * 1000));
function waitFor(fn, secs = 6) {
    const end = Date.now() + secs * 1000;
    while (Date.now() < end) {
        if (fn()) return true;
        pause(0.3);
    }
    return false;
}
function blackFraction(y0, y1) {
    const { w, bpp, px } = decodePng(adb("exec-out", "screencap", "-p").stdout);
    let n = 0, k = 0;
    for (let y = Math.trunc(y0); y < Math.trunc(y1); y += 9) {
        for (let x = 0; x < w; x += 9) {
            const i = (y * w + x) * bpp;
            n++;
            if (px[i] === 0 && px[i + 1] === 0 && px[i + 2] === 0) k++;
        }
    }
    return k / Math.max(n, 1);
}
const I = (v) => String(Math.trunc(v));
const insetsOf = (line) => { const m = /insets ([-\d.,]+)$/.exec(line); return m ? m[1].split(",").map(Number) : [0, 0, 0, 0]; };

adb("shell", "am", "force-stop", PKG);
adb("shell", "am", "start", "-W", "-n", ACT);
check(waitFor(() => last("screen") && last("state")), "the app starts and logs its screen and state");
const m = SCREEN_RE.exec(last("screen"));
check(m !== null, `the screen line parses (${JSON.stringify(last("screen"))})`);
if (!m) std.exit(1);
const lw = Number(m[1]), lh = Number(m[2]), pw = Number(m[3]), ph = Number(m[4]), s = Number(m[5]);
const ins = m[7].split(",").map(Number);
check(Math.abs(lw * s - pw) < 1 && Math.abs(lh * s - ph) < 1, `logical size x scale is the pixel size (${lw} x ${s} = ${pw})`);
check(ins.every((v) => v >= 0), `insets are never negative (${ins})`);
const navY = (lh - ins[3] - 22) * s;
const tab = (k) => I((ins[0] + (lw - ins[0] - ins[2]) * (k + 0.5) / 3) * s);
for (const [k, want] of [[1, 1], [2, 2], [0, 0]]) {
    adb("shell", "input", "tap", tab(k), I(navY));
    check(waitFor(() => state().nav === want), `a tap on nav item ${k} selects it (nav ${state().nav})`);
}
const fab = [I((lw - ins[2] - 16 - 28) * s), I((lh - ins[3] - 44 - 16 - 28) * s)];
const before = state().toasts ?? 0;
adb("shell", "input", "tap", ...fab);
check(waitFor(() => (state().toasts ?? 0) === before + 1), "a tap on the FAB runs its action once");
adb("shell", "input", "tap", tab(1), I(navY));
waitFor(() => state().nav === 1);
const x = I(lw / 2 * s), y0 = Math.trunc((ins[1] + 420) * s), y1 = Math.trunc((ins[1] + 120) * s);
adb("shell", "input", "swipe", x, String(y0), x, String(y1), "120");
pause(2);
check(state().sel === -1, `a swipe over the list scrolls it and selects nothing (sel ${state().sel})`);
adb("shell", "input", "tap", x, I((ins[1] + 200) * s));
check(waitFor(() => (state().sel ?? -1) > 8), `a tap after the fling selects a row far down the list (sel ${state().sel})`);
const held = state().sel;
adb("shell", "input", "swipe", x, I((ins[1] + 250) * s), x, I((ins[1] + 250) * s), "900");
pause(1.5);
check(state().sel !== held, `a long press selects the row it holds (${held} -> ${state().sel})`);
adb("shell", "input", "keyevent", "KEYCODE_HOME");
pause(2);
adb("shell", "am", "start", "-n", ACT);
pause(2.5);
const frac = blackFraction(ins[1] * s, (lh - ins[3]) * s);
check(frac < 0.05, `after HOME and relaunch the app repaints without input (${Math.round(frac * 100)}% of the app area black)`);
adb("shell", "input", "tap", tab(2), I(navY));
waitFor(() => state().nav === 2);
pause(1);
let fieldY = null;
for (let yy = Math.trunc((ins[1] + 150) * s); yy < Math.trunc((lh - ins[3] - 60) * s); yy += Math.trunc(10 * s)) {
    adb("shell", "input", "tap", I(lw * 0.3 * s), String(yy));
    pause(0.8);
    if (adbText("shell", "dumpsys", "input_method").includes("mInputShown=true")) { fieldY = yy; break; }
}
check(fieldY !== null, "a tap on the text field raises the on-screen keyboard");
pause(1);
const kb = insetsOf(last("screen"));
check(kb[3] > ins[3] + 100, `the keyboard becomes the bottom inset (${ins[3]} -> ${kb[3]} pt)`);
adb("shell", "input", "text", "Zed");
check(waitFor(() => String(state().name ?? "").endsWith("Zed")), `typing through the IME reaches the field (${JSON.stringify(state().name)})`);
adb("shell", "input", "tap", I(lw / 2 * s), I((ins[1] + 120) * s));
pause(2);
const back = insetsOf(last("screen"));
check(back[3] === ins[3], `tapping elsewhere hides the keyboard and restores the inset (${back[3]})`);
print(`android smoke: ${fails} failure(s)`);
std.exit(fails ? 1 : 0);
