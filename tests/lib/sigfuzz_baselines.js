// tests/lib/sigfuzz_baselines.js -- what sigfuzz must not call, and the valid
// baselines it cannot infer from a signature alone.
//
// Why baselines matter: sigfuzz varies ONE argument at a time and keeps the
// others at their baseline. If any baseline argument is invalid, every
// variant is refused at that argument and the edge values of the others never
// reach the code behind it. A name listed as "never reached" in a run is a
// name whose baseline belongs here.

// SKIP: by bare name (any module) or "module.Label". Network I/O, process
// spawning and filesystem mutation outside the scratch directory are not
// input-handling questions; the suites that own them test them with peers.
export const SKIP = {
    App: 1, WsClient: 1, HTTPServer: 1, HTTPServerAsync: 1, TCPServer: 1, UDPSocket: 1, DNSServer: 1,
    TCPProxy: 1, HTTPClient: 1, Fetcher: 1, fetch: 1, Crawl: 1, DNSResolver: 1, Redis: 1,
    PostgreSQL: 1, SQLite: 1, Watcher: 1, Exec: 1, Spawn: 1, Which: 1, connect: 1, connectHappy: 1,
    listen: 1, serve: 1, resolve: 1, lookup: 1, request: 1,
    removeAll: 1, remove: 1, rename: 1, move: 1, symlink: 1, chmod: 1, chown: 1, chDir: 1,
    setEnv: 1, unsetEnv: 1, exit: 1, kill: 1, sleep: 1, sleepSync: 1, setSigHandler: 1,
    "sys.Spawn#new": 1, "file.makeDir": 1, "file.writeFile": 1, "file.appendFile": 1, "file.copyFile": 1,
    "file.makeTempDir": 1, "file.makeTempFile": 1, "file.truncate": 1, "file.link": 1,
    "compress.ZipExtractAll": "writes a tree; covered by its own suite with a scratch root",
    "bench.run": 1, "bench.compare": 1,
    "scrape.Sitemap.list": "fetches the sitemap over the network; tests/scrape/test_scrape_sitemap.js serves one",
    "sys.setUid": "changes the identity of the process running the suite",
    "sys.setGid": "changes the identity of the process running the suite",
    "uring.open": 1,
    "file.async.makeDir": "mutates the tree like its sync twin, after the call returns; tests/file/test_file_async_fs.js runs it against sync on twin trees",
    "file.async.copyFile": "same as file.async.makeDir", "file.async.remove": "same as file.async.makeDir", "file.async.removeAll": "same as file.async.makeDir",
    "file.async.rename": "same as file.async.makeDir", "file.async.move": "same as file.async.makeDir", "file.async.symlink": "same as file.async.makeDir",
    "file.async.chmod": "same as file.async.makeDir; a late chmod of the scratch directory would break the rest of the sweep",
    "sdl.quit": "shuts SDL down under the rest of the sweep; tests/sdl/test_sdl.js covers it, including its refusal while objects are open",
    "sdl.setClipboardText": "would replace the clipboard of whoever runs the suite; tests/sdl/test_sdl.js covers it under the dummy driver",
    "raylib.Window#mode2D": "only legal inside frame() or RenderTexture#draw(), so a bare call is always a correct refusal; tests/raylib/test_raylib.js drives it inside both",
    "raylib.Window#mode3D": "same as mode2D",
    "raylib.Window#blendMode": "same as mode2D; tests/raylib/test_raylib_more.js drives it inside a frame and a RenderTexture",
    "raylib.Window#scissor": "same as mode2D; tests/raylib/test_raylib_more.js drives it inside a RenderTexture",
    "raylib.closeAudio": "closes the device under the rest of the sweep; tests/raylib/test_raylib.js covers it",
    "raylib.Font": "needs a TTF on the host; tests/raylib/test_raylib.js loads a system font where one exists",
    "raylib.loadFont": "needs a TTF on the host; tests/raylib/test_raylib.js covers the refusal and, where a system font exists, the load",
    "ui.Context#loadFont": "declared for TRUSTED fonts only (stb_truetype does not validate); fuzzing it is outside its contract",
    "sdl.Window#setOpacity": "the dummy video driver has no window opacity, so every call ends in SDL's refusal; tests/sdl/test_sdl_media.js covers its range checks",
    "sdl.Window#maximize": "the dummy video driver has no window manager, so SDL refuses it as unsupported; tests/sdl/test_sdl_controls.js accepts either outcome",
    "sdl.Window#minimize": "same as maximize",
    "sdl.Window#restore": "same as maximize",
    "sdl.Window#flash": "same as maximize",
    "sdl.Window#setResizable": "same as maximize; tests/sdl/test_sdl_controls.js covers the boolean refusal",
    "sdl.Window#setBordered": "same as maximize",
    "sdl.Window#setAlwaysOnTop": "same as maximize",
    "sdl.setCursor": "the dummy video driver has no system cursors, so SDL refuses every name; tests/sdl/test_sdl_controls.js covers the name checks",
    "crypto.RSA.generate": "keygen per call is seconds of CPU; the RSA rows use one cached key",
    "crypto.Argon2id.hash": "memory-hard by design: each edge would allocate its m_cost",
    "crypto.Argon2id.verify": "same cost as hash",
    "crypto.Scrypt": "memory-hard by design",
    "docdb.Collection#new": "a Collection comes from db.collection(name); the constructor only refuses (tests/docdb cover it)",
    "ai.MessageStream#new": "a MessageStream comes from client.messages.stream(body); the constructor only refuses (tests/ai cover it)",
    "ai.MessageStream#next": "network I/O; tests/ai/test_ai_anthropic.js streams from a local App peer",
    "ai.MessageStream#finalMessage": "network I/O; tests/ai/test_ai_anthropic.js streams from a local App peer",
    "ai.MessageStream#finalText": "network I/O; tests/ai/test_ai_anthropic.js streams from a local App peer",
    "ai.BatchResults#new": "a BatchResults comes from client.messages.batches.results(id); the constructor only refuses (tests/ai/test_ai_batch_results.js covers it)",
    "ai.BatchResults#next": "network I/O; tests/ai/test_ai_batch_results.js reads results from a local TCP peer",
    "ai.ChatCompletionStream#new": "a ChatCompletionStream comes from client.chat.completions.stream(body); the constructor only refuses (tests/ai/test_ai_open_ai.js covers it)",
    "ai.ChatCompletionStream#next": "network I/O; tests/ai/test_ai_open_ai.js streams from a local App peer",
    "ai.ChatCompletionStream#finalChatCompletion": "network I/O; tests/ai/test_ai_open_ai.js streams from a local App peer",
    "ai.ChatCompletionStream#finalContent": "network I/O; tests/ai/test_ai_open_ai.js streams from a local App peer",
    "docdb.ChangeStream#new": "a ChangeStream comes from watch(); the constructor only refuses (tests/docdb cover it)",
    ...(std.getenv("DYNAJS_UI_VISIBLE") === "1" ? {} : Object.fromEntries(
        ["toggleFullscreen", "toggleBorderless", "maximize", "minimize", "restore", "focus", "setTopmost", "setDecorated", "setPosition", "setSize", "setOpacity"].map((m) => [
            "raylib.Window#" + m,
            "changes the real window state, which the OS shows on screen even for a hidden window; DYNAJS_UI_VISIBLE=1 sweeps it",
        ]),
    )),
};

// Names whose work is legitimately linear in a caller array's declared length:
// a sparse array of length 2^32-1 asks them for billions of steps, and they do
// them. Every other name must refuse such an array before working (that is
// what the probe checks), so each entry here says why it is exempt.
// Names with a documented work budget above the default per-call allowance:
// { ms, why }. The allowance is the budget's ceiling, so a call that runs
// past it is still a failure.
const DIFF_BUDGET = { ms: 6000, why: "Myers diff spends up to 2^31 diagonal steps before refusing as too dissimilar (~2 s here)" };
export const SLOW_OK = {
    "matcher.DiffChars": DIFF_BUDGET, "matcher.DiffWords": DIFF_BUDGET, "matcher.DiffLines": DIFF_BUDGET,
    "random.Random#bytes": { ms: 6000, why: "produces the n bytes it is asked for: the 2^30 cap admits the 1e9 edge, filled in place at ~5 GB/s (0.22 s idle on arm64; 2.7 s on the loaded Linux gate VM with the old scratch copy, which made peak memory 2n)" },
    "bytes.Bytes.alloc": { ms: 8000, why: "zero-fills the requested length by contract, like new Uint8Array(n): the 2^31-1 edge touches 2 GiB (~1 s idle, 2.2 s on the loaded Linux gate VM)" },
};

// Option fields whose value is a resource the caller explicitly asks for: an
// edge value above the cap is skipped, because honouring it is the contract.
// preallocate reserves real disk on Linux (fallocate): the 1e9/2^32 edges
// filled the gate VM's disk and broke unrelated suites.
export const FIELD_MAX = { preallocate: 1 << 20 };

export const HUGE_OK = {
    "random.Random#shuffle": "an in-place Fisher-Yates over the array the caller gave; 16 ns per slot",
    "async.pmap": "one call of fn per index by contract (fn(item, index, items)), at bounded concurrency",
};

import * as os from "os";
import * as std from "std";

const u8 = (...b) => new Uint8Array(b);

// Expensive or stateful fixtures are built once per run and shared.
const memo = new Map();
const memoOnce = (key, make) => { if (!memo.has(key)) memo.set(key, make()); return memo.get(key); };
let uniq = 0;
// A fresh file of its own per call: a File method that removes, moves or
// truncates must not take the fixture away from every method after it.
function docdbColl(ns) {
    const c = new ns.Database("fz").collection("c");
    c.insertOne({ _id: 1, a: 1 });
    c.createIndex({ a: 1 });
    return c;
}
// a valid 1-collection, 1-document dyna:docdb snapshot (written by Database.persist; the format is in dynajs.d.ts)
const DOCDB_SNAP = [68, 89, 78, 65, 68, 79, 67, 0, 1, 0, 0, 0, 0, 0, 0, 0, 203, 98, 118, 217, 161, 1, 0, 0, 2, 0, 0, 0, 47, 159, 194, 66, 1, 0, 0, 0, 63, 0, 0, 0, 0, 0, 0, 0, 53, 132, 97, 248, 63, 0, 0, 0, 16, 118, 0, 1, 0, 0, 0, 4, 99, 111, 108, 108, 101, 99, 116, 105, 111, 110, 115, 0, 14, 0, 0, 0, 2, 48, 0, 2, 0, 0, 0, 99, 0, 0, 3, 117, 115, 101, 114, 0, 18, 0, 0, 0, 2, 110, 97, 109, 101, 0, 3, 0, 0, 0, 102, 122, 0, 0, 0, 2, 0, 0, 0, 96, 0, 0, 0, 0, 0, 0, 0, 160, 97, 220, 8, 75, 0, 0, 0, 2, 110, 97, 109, 101, 0, 2, 0, 0, 0, 99, 0, 18, 99, 111, 117, 110, 116, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 109, 101, 116, 97, 0, 37, 0, 0, 0, 16, 114, 105, 110, 103, 67, 97, 112, 97, 99, 105, 116, 121, 0, 0, 16, 0, 0, 4, 105, 110, 100, 101, 120, 101, 115, 0, 5, 0, 0, 0, 0, 0, 0, 21, 0, 0, 0, 16, 95, 105, 100, 0, 1, 0, 0, 0, 16, 97, 0, 1, 0, 0, 0, 0, 68, 89, 78, 65, 69, 78, 68, 0, 247, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 128, 175, 156, 53];
function docdbSnap(scratch) {
    return sdlFile(scratch, "docdb-base.dds", new Uint8Array(DOCDB_SNAP));
}
function freshFile(scratch, text = "line one\nline two\n") {
    const path = `${scratch}/f${++uniq}.txt`;
    const f = std.open(path, "w");
    f.puts(text);
    f.close();
    return path;
}
const cryptoNs = () => memoOnce("crypto-ns", () => import("dyna:crypto"));
const NONCE = new Uint8Array(12).fill(5);
const AEAD_KEY = new Uint8Array(32).fill(3);
const bytesOf = (s) => new TextEncoder().encode(s);
const once = (f) => { let v, done = false; return () => (done ? v : ((done = true), (v = f()))); };

const memSink = () => ({ n: 0, closed: false, write(b) { this.n += b.length; return Promise.resolve(b.length); },
    flush() { return Promise.resolve(); }, close() { this.closed = true; }, [Symbol.dispose]() { this.close(); } });
const memSource = (s) => { const b = bytesOf(s); let off = 0; return { read(buf) { const n = Math.min(buf.length, b.length - off); buf.set(b.subarray(off, off + n)); off += n; return Promise.resolve(n); }, close() { off = b.length; } }; };
function smallFrame(ns) {
    return new ns.DataFrame({ x: new Float64Array([1, 2, 3]), y: new Float64Array([3, 2, 1]), w: new Float64Array([1, 1, 1]),
        t: new Float64Array([0, 2, 4]), p: new Int32Array([0, 1, 2]), k: new Int32Array([0, 1, 2]), o: new Int32Array([1, 3, 5]), s: ["a", "b", "c"] });
}

// ---- per-module parameter baselines, by parameter NAME ---------------------
// (pname, kinds, ctx) -> a valid value, or undefined to fall back to the type
// drawTexture only accepts a texture of its own renderer: build one from the
// shared Renderer instance (once), or every call is a correct refusal.
let sdlTexture = null;
function sdlTextureOf(ctx) {
    const r = ctx.instanceOf("Renderer");
    if (!sdlTexture || sdlTexture.closed) sdlTexture = new ctx.ns.Texture(r, 4, 4);
    return sdlTexture;
}
// loadWAV and loadTexture need a real file: a 4-sample s16 WAV and a 1x1 BMP in the sweep's scratch dir.
function sdlFile(scratch, name, bytes) {
    const path = scratch + "/" + name, f = std.open(path, "wb");
    f.write(bytes.buffer, 0, bytes.length);
    f.close();
    return path;
}
function sdlWav(scratch) {
    const b = new Uint8Array(52), dv = new DataView(b.buffer);
    const tag = (o, t) => { for (let i = 0; i < 4; i++) b[o + i] = t.charCodeAt(i); };
    tag(0, "RIFF"); dv.setUint32(4, 44, true); tag(8, "WAVE"); tag(12, "fmt "); dv.setUint32(16, 16, true);
    dv.setUint16(20, 1, true); dv.setUint16(22, 1, true); dv.setUint32(24, 8000, true); dv.setUint32(28, 16000, true);
    dv.setUint16(32, 2, true); dv.setUint16(34, 16, true); tag(36, "data"); dv.setUint32(40, 8, true);
    return sdlFile(scratch, "sigfuzz.wav", b);
}
function sdlBmp(scratch) {
    const b = new Uint8Array(58), dv = new DataView(b.buffer);
    b[0] = 0x42; b[1] = 0x4d; dv.setUint32(2, 58, true); dv.setUint32(10, 54, true); dv.setUint32(14, 40, true);
    dv.setInt32(18, 1, true); dv.setInt32(22, 1, true); dv.setUint16(26, 1, true); dv.setUint16(28, 24, true); dv.setUint32(34, 4, true);
    b[54] = 30; b[55] = 20; b[56] = 10;
    return sdlFile(scratch, "sigfuzz.bmp", b);
}
// raylib drives one window per process: the shared Window is reopened whenever a hostile argument
// closed it, and every other receiver is made under the window that is open now.
let rlWin = null;
function rlWindow(ns) {
    if (!rlWin || rlWin.closed) rlWin = new ns.Window(32, 32, "sigfuzz", { hidden: true });
    return rlWin;
}
function rlReceiver(ns, cname, scratch) {
    rlWindow(ns);
    switch (cname) {
        case "Window": return rlWin;
        case "Texture": return new ns.Image(4, 4, 0xff0000ff).toTexture();
        case "RenderTexture": return new ns.RenderTexture(8, 8);
        case "Image": return new ns.Image(4, 4, 0x00ff00ff);
        case "Sound": return ns.loadSound(sdlWav(scratch));
        case "Music": return ns.loadMusic(sdlWav(scratch));
        case "AudioStream": return new ns.AudioStream(44100, 1);
        default: return undefined;
    }
}
// dyna:ui: a Context with image handle 1 registered (image(), iconButton() and avatar() have one) and one
// frame laid out (pixels() and pixelsInto() read a finished frame).
function uiContext(ns) {
    const c = new ns.Context({ width: 96, height: 64, clock: "manual" });
    c.addImage(new Uint8Array(4 * 4 * 4).fill(200), 4, 4);
    c.frame(() => {});
    return c;
}
// dyna:ui widgets exist only inside frame(): a call on a Context runs inside one, except the names that
// are refused there (theme changes, resize, idle, removeImage, frame itself) or are input and readback.
const UI_OUTSIDE = new Set(["frame", "theme", "removeImage", "resize", "idle", "close", "dispose", "loadFont", "addImage", "updateImage", "pointer",
    "key", "type", "composition", "setTime", "pixels", "pixelsInto", "geometry", "atlas", "stats", "lastRect", "toast"]);
// dragSource and dropTarget mark the widget drawn before them and refuse a frame without one.
const UI_AFTER_WIDGET = new Set(["dragSource", "dropTarget"]);
export const CALL = {
    ui: (fn, self, args) => {
        if (!self || typeof self.frame !== "function" || UI_OUTSIDE.has(fn.name)) return fn.apply(self, args);
        let r;
        self.frame(() => { if (UI_AFTER_WIDGET.has(fn.name)) self.button("w"); r = fn.apply(self, args); });
        return r;
    },
};
export const PARAMS = {
    raylib: (p, kinds, ctx) => (rlWindow(ctx.ns), p === "texture") ? rlReceiver(ctx.ns, "Texture") : p === "font" ? null : ({ width: 16, height: 16, x: 2, y: 3, x1: 0, y1: 0, x2: 8, y2: 8, x3: 0, y3: 8, z: 0, z1: 0, z2: 1, cx: 8, cy: 8, radius: 4, rx: 4, ry: 2, color: 0xff0000ff, tint: 0xffffffff, thick: 1, roundness: 0.5, segments: 4, sides: 5, rotation: 0, length: 2, slices: 4, spacing: 1, fontSize: 10, text: "x", key: 65, button: 0, gamepad: 0, axis: 0, size: 16, fps: 60, title: "fuzz", sampleRate: 44100, channels: 1, volume: 0.5, pitch: 1, pan: 0.5, seconds: 0, on: false, mode: "point", level: "error", r: 1, g: 2, b: 3, a: 255, src: { x: 0, y: 0, width: 1, height: 1 }, dst: { x: 0, y: 0, width: 2, height: 2 }, originX: 0, originY: 0, camera: {}, draw: () => {}, pixels: new Uint8Array(16 * 16 * 4), samples: new Float32Array(64), fill: 0xff, leftMotor: 0.5, rightMotor: 0.5, topLeft: 0xff0000ff, bottomLeft: 0x00ff00ff, topRight: 0x0000ffff, bottomRight: 0xffffffff, points: new Float32Array([0, 0, 8, 0, 8, 8]) })[p],
    ui: (p, kinds) => {
        const k = kinds.map((x) => x.k);
        if (p === "selected" || p === "checked" || p === "on") return k.includes("boolean") ? true : 0;
        if (p === "value") return k.includes("array") ? [0.2, 0.6] : 1;
        if (p === "rgba") return k.includes("number") ? 0x336699ff : new Uint8Array(4 * 4 * 4);
        return ({ handle: 1, image: 1, fn: () => {}, first: () => {}, second: () => {}, date: { year: 2026, month: 10, day: 30 }, items: ["a", "b"],
            lines: ["one", "two"], values: [1, 3, 2], label: "w", title: "t", text: "t", message: "m", name: "left", min: 0, max: 1, page: 1, pages: 3,
            current: 0, ratio: 0.5, fraction: 0.5, seconds: 0, cursor: 0, size: 16, color: 0x3d7effff, tint: 0xffffffff, state: { x: 4, y: 4 },
            time: { hour: 9, minute: 30 }, tags: ["a", "b"], sections: ["a", "b"], open: 0, suggestions: ["a", "b"], payload: "p" })[p];
    },
    sdl: (p, kinds, ctx) => p === "texture" ? sdlTextureOf(ctx) : ({ title: "fuzz", width: 16, height: 16, x: 2, y: 3, x1: 0, y1: 0, x2: 8, y2: 8, r: 10, g: 20, b: 30, a: 255, alpha: 128, key: 97, name: "A", subsystems: ["events"], event: { type: "user", code: 1 }, max: 8, on: false, gain: 1, mode: "blend", value: "1", text: "x", pixels: new Uint8Array(16 * 16 * 4), samples: new Float32Array(64) })[p],
    vec: (p, kinds, ctx) => (p === "path" && ctx.Path ? new ctx.Path(`${ctx.scratch}/index${++uniq}.vec`) : undefined) ?? ({ opts: { dims: 4 }, id: 1, ids: Uint32Array.of(7, 8), vectors: Float32Array.of(1, 0, 0, 0, 0, 1, 0, 0), query: Float32Array.of(1, 1, 0, 0), queries: Float32Array.of(1, 1, 0, 0, 0, 0, 1, 1), k: 2 })[p],
    semver: (p) => ({ rangeString: "^1.2.0", version: "1.2.3", v: "1.2.3", a: "1.2.3", b: "2.0.0-rc.1", range: "^1.0.0", rangeA: "^1.0.0", rangeB: ">=1.2.0 <2.0.0", release: "minor", identifier: "beta", versions: ["1.0.0", "1.2.3", "2.0.0"] })[p],
    uuid: (p) => ({ uuid: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", id: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", a: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", b: "6ba7b811-9dad-11d1-80b4-00c04fd430c8", namespace: "6ba7b810-9dad-11d1-80b4-00c04fd430c8", name: "example.com", bytes: new Uint8Array(16).fill(7), ulid: "01ARZ3NDEKTSV4RRFFQ69G5FAV" })[p],
    time: (p, kinds, ctx) => p === "duration" ? new ctx.ns.Duration({ days: 1, hours: 2 }) : ({ start: "2024-01-01T00:00:00Z", end: "2024-01-10T00:00:00Z", str: "2024-02-29T12:34:56.789Z", text: "2024-02-29 12:34:56", iso: "2024-02-29T12:34:56Z", layout: "2006-01-02", value: "2024-02-29", zone: "UTC", tz: "UTC", year: 2024, month: 2, day: 29, hour: 12, minute: 34, second: 56, rule: "FREQ=DAILY;COUNT=3" })[p],
    // Decimal operands are decimal strings; Money takes integer minor units and an ISO code.
    decimal: (p, kinds) => {
        if (/^(value|x|str)$/.test(p) && kinds.some((k) => k.k === "string")) return p === "str" ? "12.34" : "2.5";
        return { minorUnits: 1234, currency: "USD", n: 3, shares: [1, 2, 1], digits: 2, dp: 2, places: 2 }[p];
    },
    mathx: (p, kinds) => {
        const isArr = kinds.some((k) => k.k === "array" || k.k === "typed" || k.k === "obj");
        if ((p === "a" || p === "b") && kinds.some((k) => k.k === "bigint"))
            return kinds.some((k) => k.k === "number") ? (p === "a" ? 12 : 18) : (p === "a" ? 12n : 18n);
        if ((p === "a" || p === "b") && isArr) return p === "a" ? [1, 2, 4, 8, 3] : [2, 1, 5, 9, 4];
        return { q: 0.25, text: "x * 2 + sin(y)", vars: { x: 3, y: 0.5 } }[p];
    },
    net: (p) => ({ name: "requests_total", value: 3, addr: "192.0.2.10", a: "192.0.2.10", b: "2001:db8::1", prefix: "192.0.2.0/24", cidr: "2001:db8::/32", ip: "192.0.2.10" })[p],
    netip: (p) => ({ name: "requests_total", value: 3, addr: "192.0.2.10", a: "192.0.2.10", b: "2001:db8::1", prefix: "192.0.2.0/24", cidr: "2001:db8::/32", ip: "192.0.2.10" })[p],
    metrics: (p) => ({ name: "requests_total", value: 3, addr: "192.0.2.10", a: "192.0.2.10", b: "2001:db8::1", prefix: "192.0.2.0/24", cidr: "2001:db8::/32", ip: "192.0.2.10" })[p],
    dns: (p) => ({ name: "requests_total", value: 3, addr: "192.0.2.10", a: "192.0.2.10", b: "2001:db8::1", prefix: "192.0.2.0/24", cidr: "2001:db8::/32", ip: "192.0.2.10" })[p],
    proxy: (p) => ({ name: "requests_total", value: 3, addr: "192.0.2.10", a: "192.0.2.10", b: "2001:db8::1", prefix: "192.0.2.0/24", cidr: "2001:db8::/32", ip: "192.0.2.10" })[p],
    cli: (p, kinds, ctx) => (p === "sub" ? new ctx.ns.Command("sub") : { flags: "-q, --quiet", args: ["-v", "--name", "x"] }[p]),
    csv: (p) => ({ columns: ["a", "b"], row: 0, value: "v", column: "a", columnIndex: 0, oldName: "a", newName: "z", defaultValue: "d" })[p],
    // index-like parameters of the structures are small valid indices; save()
    // takes a Path, a fresh file in the scratch directory per call
    structures: (p, kinds, ctx) => {
        if (p === "path" && ctx.Path) return new ctx.Path(`${ctx.scratch}/persist${++uniq}.dpf`);
        return { u: 0, v: 1, w: 2, src: 0, dst: 2, x: 0, y: 1, lo: 0, hi: 2 }[p];
    },
    bytes: (p) => ({ label: "utf-8", text: "héllo", offset: 0, value: 7 })[p],
    file: (p, kinds, ctx) => {
        const ns = ctx.ns;
        if (/^dest$/.test(p) && ns && typeof ns.Path === "function") return new ns.Path(`${ctx.scratch}/dest${++uniq}.txt`);
        if (/^(path|dir|target|from|to|src|dst|root)$/i.test(p) && ns && typeof ns.Path === "function")
            return new ns.Path(ctx.scratch + (/dir|root/i.test(p) ? "" : "/seed.txt"));
        return { pattern: "*.txt", glob: "*.txt" }[p];
    },
    encoding: (p) => ({ expression: "$.a[0]", expr: "$.a[0]", path: "$.a", label: "utf-8" })[p],
    crypto: (p) => ({ algorithm: "sha256", alg: "sha256", hash: "sha256", md: "sha256", curve: "P-256" })[p],
    hash: (p) => ({ algorithm: "sha256", alg: "sha256" })[p],
    stream: (p) => {
        if (p === "src") return memSource("a\nb\n{\"k\":1}\n");
        if (p === "dst" || p === "sink") return memSink();
        return undefined;
    },
    json: (p) => ({ pointer: "/a/0", path: "/a/0", doc: { a: [1, 2] }, document: { a: [1, 2] }, value: 3 })[p],
    dataframe: (p, kinds, ctx) => {
        const rows = 8;
        if (p === "other") return smallFrame(ctx.ns);
        const v = {
            time: "t", tau: 2, loCol: "x", hiCol: "y", leftKey: "k", rightKey: "k", leftTime: "o", rightTime: "o",
            index: "k", columns: "s", values: "x", agg: "sum", idVars: ["k"], valueVars: ["x", "y"], how: "inner",
            col: "x", a: "x", b: "y", x: "x", y: "y", key: "k", val: "x", value: "x", valueCol: "x", weightCol: "w",
            timeCol: "t", position: "p", names: ["x", "y"], cols: ["x", "y"], mask: new Uint8Array(rows).fill(1),
            q: 0.5, qs: [0.25, 0.5], k: 3, n: 3, w: 3, bins: 4, buckets: 3, periods: 1, alpha: 0.5, sep: ",",
            lo: -1, hi: 5, fill: 0, start: 0, end: 4, size: 4, every: 2,
        }[p];
        return v;
    },
    ml: (p, kinds, ctx) => {
        const X = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 2], [2, 3], [3, 2], [3, 3]];
        if (p === "path" && ctx.Path) return new ctx.Path(ctx.scratch + "/model.bin");
        if (p === "out") return new Float64Array(64);
        return { X, x: X, y: [0, 0, 0, 0, 1, 1, 1, 1], yTrue: [0, 1, 1, 0], yPred: [0, 1, 0, 0], yScore: [0.1, 0.9, 0.4, 0.2], labels: [0, 1],
            grid: { alpha: [0.1, 1] } }[p];
    },
};

// ---- explicit baseline argument lists: "module.Label" -> (ctx, scratch, ns) => args
const PROTO_SCHEMA = { fields: [{ name: "id", number: 1, type: "int64" }, { name: "name", number: 2, type: "string" }, { name: "tags", number: 3, type: "string", repeated: true }] };
const zipOf = (ns) => ns.ZipPack([{ name: "a.txt", data: bytesOf("A") }, { name: "dir/b.txt", data: bytesOf("bee ".repeat(64)), method: "deflate" }]);
const PKCE_VERIFIER = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";

// Keys are generated once per run; every signature row signs with them.
async function keys() {
    const c = await cryptoNs();
    return memoOnce("keys", () => {
        const ed = c.Ed25519Generate(), x1 = c.X25519Generate(), x2 = c.X25519Generate();
        const rsa = c.RSA.generate(2048), ec1 = c.ECDSA.generate("P-256"), dh1 = c.ECDH.generate("P-256"), dh2 = c.ECDH.generate("P-256");
        const msg = "the message";
        return {
            ed, x1, x2, rsa, ec1, dh1, dh2, msg,
            edSig: c.Ed25519Sign(ed.privateKey, msg),
            rsaSig: c.RSA.sign("sha256", rsa.privateKey, msg),
            pssSig: c.RSA.signPSS("sha256", rsa.privateKey, msg),
            ecSig: c.ECDSA.sign("sha256", ec1.privateKey, msg),
            edPem: c.Ed25519PemFromRaw({ privateKey: ed.privateKey, publicKey: ed.publicKey }).privateKey,
            xPem: c.X25519PemFromRaw({ privateKey: x1.privateKey, publicKey: x1.publicKey }).privateKey,
            cert: c.X509.generateSelfSigned({ key: ec1.privateKey, subject: "sigfuzz", sans: ["example.com", "127.0.0.1"] }),
        };
    });
}
// The AEAD and asymmetric rows, for dyna:crypto (and any module re-exporting them).
function cryptoBaselines(m) {
    const k = (f) => async () => f(await keys(), await cryptoNs());
    const aead = (C) => ({
        [`${m}.${C}#open`]: k((K, c) => [NONCE, new c[C](AEAD_KEY).seal(NONCE, "hello", "aad"), "aad"]),
        [`${m}.${C}#sealInto`]: () => [new Uint8Array(64), NONCE, "hello", "aad"],
        [`${m}.${C}#openInto`]: k((K, c) => [new Uint8Array(64), NONCE, new c[C](AEAD_KEY).seal(NONCE, "hello", "aad"), "aad"]),
    });
    return {
        ...aead("AESGCM"), ...aead("ChaCha20Poly1305"),
        [`${m}.Ed25519Sign`]: k((K) => [K.ed.privateKey, K.msg]),
        [`${m}.Ed25519Verify`]: k((K) => [K.ed.publicKey, K.msg, K.edSig]),
        [`${m}.X25519Derive`]: k((K) => [K.x1.privateKey, K.x2.publicKey]),
        [`${m}.Ed25519PemToRaw`]: k((K) => [K.edPem]),
        [`${m}.Ed25519PemFromRaw`]: k((K) => [{ privateKey: K.ed.privateKey, publicKey: K.ed.publicKey }]),
        [`${m}.X25519PemToRaw`]: k((K) => [K.xPem]),
        [`${m}.X25519PemFromRaw`]: k((K) => [{ privateKey: K.x1.privateKey, publicKey: K.x1.publicKey }]),
        [`${m}.RSA.sign`]: k((K) => ["sha256", K.rsa.privateKey, K.msg]),
        [`${m}.RSA.verify`]: k((K) => ["sha256", K.rsa.publicKey, K.msg, K.rsaSig]),
        [`${m}.RSA.signPSS`]: k((K) => ["sha256", K.rsa.privateKey, K.msg, { saltLen: "auto" }]),
        [`${m}.RSA.verifyPSS`]: k((K) => ["sha256", K.rsa.publicKey, K.msg, K.pssSig]),
        [`${m}.RSA.OAEP.seal`]: k((K) => [K.rsa.publicKey, "hello"]),
        [`${m}.RSA.OAEP.open`]: k((K, c) => [K.rsa.privateKey, c.RSA.OAEP.seal(K.rsa.publicKey, "hello")]),
        [`${m}.X509.parse`]: k((K) => [K.cert]),
        [`${m}.X509.generateSelfSigned`]: k((K) => [{ key: K.ec1.privateKey, subject: "s", days: 30, sans: ["a.example", "::1", "x@example.com"] }]),
        [`${m}.ECDSA.sign`]: k((K) => ["sha256", K.ec1.privateKey, K.msg]),
        [`${m}.ECDSA.verify`]: k((K) => ["sha256", K.ec1.publicKey, K.msg, K.ecSig]),
        [`${m}.ECDH.derive`]: k((K) => [K.dh1.privateKey, K.dh2.publicKey]),
    };
}

// Request/Response live in dyna:http, Metrics in dyna:metrics (B1e: dyna:net re-exports neither).
const MP_BODY = "--XyZ\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--XyZ\r\nContent-Disposition: form-data; name=\"f\"; filename=\"f.txt\"\r\nContent-Type: text/plain\r\n\r\nhello\r\n--XyZ--\r\n";
function fetchBaselines(m) {
    return {
        [`${m}.Response#instance`]: (c, s, ns) => new ns.Response('{"a":[1,2]}', { status: 200, headers: { "content-type": "application/json" } }),
        [`${m}.Request#instance`]: (c, s, ns) => new ns.Request("http://example.com/p?q=1", { method: "POST", body: '{"a":[1,2]}', headers: { "content-type": "application/json" } }),
        // each metric kind gets its own name: a name is bound to the kind that first used it
        [`${m}.Metrics.gauge`]: () => ["sigfuzz_gauge", 3, { k: "v" }],
        [`${m}.Metrics.histogram`]: () => ["sigfuzz_seconds", 0.2, { k: "v" }],
        [`${m}.MultipartParse`]: () => ["multipart/form-data; boundary=XyZ", bytesOf(MP_BODY)],
    };
}

export const BASELINES = {
    "http.AbortSignal#instance": (c, s, ns) => new ns.AbortController().signal,
    "net.AbortSignal#instance": (c, s, ns) => new ns.AbortController().signal,
    "ai.Anthropic#new": () => [{ apiKey: "test-key", baseURL: "http://127.0.0.1:9", maxRetries: 0, timeout: 2000 }],
    "ai.MessageStream#instance": (c, s, ns) => new ns.Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:9", maxRetries: 0, timeout: 2000 })
        .messages.stream({ model: "sigfuzz-model", max_tokens: 1, messages: [] }),
    "ai.BatchResults#instance": (c, s, ns) => new ns.Anthropic({ apiKey: "test-key", baseURL: "http://127.0.0.1:9", maxRetries: 0, timeout: 2000 })
        .messages.batches.results("sigfuzz-batch"),
    "ai.OpenAI#new": () => [{ apiKey: "test-key", baseURL: "http://127.0.0.1:9/v1", maxRetries: 0, timeout: 2000 }],
    "ai.ChatCompletionStream#instance": (c, s, ns) => new ns.OpenAI({ apiKey: "test-key", baseURL: "http://127.0.0.1:9/v1", maxRetries: 0, timeout: 2000 })
        .chat.completions.stream({ model: "sigfuzz-model", messages: [] }),
    "sdl.Window#new": () => ["fuzz", 32, 32, { hidden: true }],
    "ui.Context#new": () => [{ width: 96, height: 64, clock: "manual" }],
    "ui.Context#instance": (c, s, ns) => uiContext(ns),
    "ui.Context#addImage": () => [new Uint8Array(4 * 4 * 4), 4, 4],
    "ui.Context#range": () => ["w", [0.2, 0.6], 0, 1],
    "ui.Context#row": () => [{}, () => {}],
    "ui.Context#column": () => [{}, () => {}],
    "ui.Context#box": () => [{}, () => {}],
    "ui.Context#wrap": () => [{}, () => {}],
    "ui.Context#card": () => ["t", {}, () => {}],
    "ui.Context#panel": () => ["w", {}, () => {}],
    "ui.Context#scroll": () => ["w", {}, () => {}],
    "ui.Context#window": () => ["t", {}, () => {}],
    "ui.Context#split": () => ["w", 0.5, {}, () => {}, () => {}],
    "ui.Context#modal": () => ["w", {}, () => {}],
    "ui.Context#drawer": () => ["w", {}, () => {}],
    "ui.Context#updateImage": () => [1, new Uint8Array(4 * 4 * 4)],
    "ui.Context#pixelsInto": () => [new Uint8Array(96 * 64 * 4)],
    "sdl.Renderer#new": (c, s, ns) => [new ns.Window("fuzz-r", 32, 32, { hidden: true })],
    "sdl.Renderer#instance": (c, s, ns) => new ns.Renderer(new ns.Window("fuzz-ri", 32, 32, { hidden: true })),
    "sdl.Texture#new": (c, s, ns) => [new ns.Renderer(new ns.Window("fuzz-t", 32, 32, { hidden: true })), 16, 16],
    "sdl.Texture#instance": (c, s, ns) => new ns.Texture(new ns.Renderer(new ns.Window("fuzz-ti", 32, 32, { hidden: true })), 16, 16),
    "sdl.AudioStream#new": () => [{ freq: 48000, channels: 1, format: "f32" }],
    "sdl.loadWAV": (c, s) => [sdlWav(s)],
    "sdl.detachVirtualGamepad": (c, s, ns) => [ns.attachVirtualGamepad("fuzz-d")],
    "sdl.Gamepad#new": (c, s, ns) => [ns.attachVirtualGamepad("fuzz-n")],
    "sdl.Gamepad#instance": (c, s, ns) => new ns.Gamepad(ns.attachVirtualGamepad("fuzz-i")),
    "sdl.Gamepad#button": () => ["a"],
    "sdl.Gamepad#axis": () => ["leftx"],
    "sdl.Gamepad#setVirtualButton": () => ["a", true],
    "sdl.Gamepad#setVirtualAxis": () => ["leftx", 0.5],
    "sdl.Gamepad#rumble": () => [0.5, 0.5, 10],
    "raylib.Window#instance": (c, s, ns) => rlReceiver(ns, "Window", s),
    "raylib.Texture#instance": (c, s, ns) => rlReceiver(ns, "Texture", s),
    "raylib.RenderTexture#instance": (c, s, ns) => rlReceiver(ns, "RenderTexture", s),
    "raylib.Image#instance": (c, s, ns) => rlReceiver(ns, "Image", s),
    "raylib.Sound#instance": (c, s, ns) => rlReceiver(ns, "Sound", s),
    "raylib.Music#instance": (c, s, ns) => rlReceiver(ns, "Music", s),
    "raylib.AudioStream#instance": (c, s, ns) => rlReceiver(ns, "AudioStream", s),
    "raylib.Window#new": (c, s, ns) => { if (rlWin && !rlWin.closed) rlWin.close(); return [32, 32, "sigfuzz", { hidden: true }]; },
    "raylib.loadTexture": (c, s, ns) => { rlWindow(ns); return [sdlBmp(s)]; },
    "raylib.loadImage": (c, s) => [sdlBmp(s)],
    "raylib.loadWave": (c, s) => [sdlWav(s)],
    "raylib.loadSound": (c, s, ns) => { rlWindow(ns); return [sdlWav(s)]; },
    "raylib.loadMusic": (c, s, ns) => { rlWindow(ns); return [sdlWav(s)]; },
    "raylib.RenderTexture#new": (c, s, ns) => { rlWindow(ns); return [8, 8]; },
    "raylib.Image#new": () => [4, 4, 0xff],
    "raylib.Image#crop": () => [0, 0, 1, 1],
    "raylib.Image#resize": () => [8, 8],
    "raylib.Image#export": (c, s) => [s + "/sigfuzz-export.png"],
    "raylib.AudioStream#new": () => [44100, 1],
    "raylib.Texture#update": () => [new Uint8Array(4 * 4 * 4)],
    "sdl.loadTexture": (c, s) => [c.instanceOf("Renderer"), sdlBmp(s)],
    "sdl.Renderer#geometry": () => [new Float32Array([0, 0, 1, 1, 1, 1, 0, 0, 8, 0, 1, 1, 1, 1, 1, 0, 0, 8, 1, 1, 1, 1, 0, 1])],
    "simd.gemv": () => [new Float32Array(2), new Float32Array(6).fill(1), new Float32Array(3).fill(1), 2, 3, 1, 0],
    "simd.gemvT": () => [new Float32Array(3), new Float32Array(6).fill(1), new Float32Array(2).fill(1), 2, 3, 0],
    "simd.gemm": () => [new Float32Array(4), new Float32Array(6).fill(1), new Float32Array(6).fill(1), 2, 2, 3, 1, 0],
    "uuid.v8": () => [{ fill: new Uint8Array(16).fill(9) }],
    "uuid.v3": () => ["6ba7b810-9dad-11d1-80b4-00c04fd430c8", "example.com"],
    "uuid.v5": () => ["6ba7b810-9dad-11d1-80b4-00c04fd430c8", "example.com"],
    "compress.gunzip": (c, s, ns) => [ns.gzip(bytesOf("hello, hello"))],
    "compress.unzstd": (c, s, ns) => [ns.zstd(bytesOf("hello, hello"))],
    "compress.unbrotli": (c, s, ns) => [ns.brotli(bytesOf("hello, hello"))],
    "compress.lz4Unframe": (c, s, ns) => [ns.lz4Frame(bytesOf("hello, hello"))],
    "compress.inflate": (c, s, ns) => [ns.deflate(bytesOf("hello, hello"))],
    "compress.ZipList": (c, s, ns) => [zipOf(ns)],
    "compress.ZipRead": (c, s, ns) => [zipOf(ns), "a.txt"],
    "compress.ZipReadAt": (c, s, ns) => [zipOf(ns), 1],
    "compress.Dictionary#decompress": (c, s, ns) => [new ns.Dictionary(["hello", "world"]).compress("hello world, hello")],
    "file.readDir": (c, s, ns) => [new ns.Path(s)],
    "file.readLink": (c, s, ns) => {
        const link = `${s}/link${++uniq}`;
        os.symlink("seed.txt", link);
        return [new ns.Path(link)];
    },
    "file.async.readLink": (c, s, ns) => {
        const link = `${s}/link${++uniq}`;
        os.symlink("seed.txt", link);
        return [new ns.Path(link)];
    },
    "file.File#instance": (c, s, ns) => new ns.File(new ns.Path(freshFile(s))),
    "file.FileReader#instance": (c, s, ns) => new ns.FileReader(new ns.Path(freshFile(s))),
    "file.FileWriter#instance": (c, s, ns) => new ns.FileWriter(new ns.Path(`${s}/w${++uniq}.txt`)),
    "decimal.Decimal#toBigInt@this": (c, s, ns) => new ns.Decimal("-9007199254740993"),
    "cli.Command#instance": (c, s, ns) => new ns.Command("app").option("-v, --verbose").option("-n, --name <s>"),
    "cli.Spinner#stop@this": (c, s, ns) => new ns.Spinner({ text: "t" }).start(),
    "random.Random#setState": (c, s, ns) => [new ns.Random(7).getState()],
    "serialize.Proto.encode": () => [{ id: 7, name: "n", tags: ["a", "b"] }, PROTO_SCHEMA],
    "serialize.Proto.decode": (c, s, ns) => [ns.Proto.encode({ id: 7, name: "n", tags: ["a", "b"] }, PROTO_SCHEMA), PROTO_SCHEMA],
    "serialize.BSONDecode": (c, s, ns) => [ns.BSONEncode({ a: 1, b: "x", c: [true, null] })],
    "serialize.Timestamp#new": () => [{ t: 1, i: 2 }],
    "serialize.Decimal128#new": () => ["1.5"],
    "docdb.Collection#instance": (c, s, ns) => docdbColl(ns),
    "docdb.Collection#insertOne": () => [{ a: 2, t: ["x"] }],
    "docdb.Collection#insertMany": () => [[{ a: 3 }, { a: 4 }]],
    "docdb.Collection#find": () => [{ a: { $gte: 1 } }, { sort: { a: -1 }, projection: { a: 1 }, skip: 0, limit: 5 }],
    "docdb.Collection#findOne": () => [{ a: 1 }, { projection: { _id: 0 } }],
    "docdb.Collection#countDocuments": () => [{ a: 1 }, { skip: 0, limit: 10 }],
    "docdb.Collection#distinct": () => ["a", { a: { $exists: true } }],
    "docdb.Collection#updateOne": () => [{ a: 1 }, { $set: { b: 1 } }, { upsert: true }],
    "docdb.Collection#updateMany": () => [{}, { $inc: { n: 1 } }, { upsert: false }],
    "docdb.Collection#replaceOne": () => [{ a: 1 }, { a: 1, r: true }, { upsert: true }],
    "docdb.Collection#deleteOne": () => [{ a: 99 }],
    "docdb.Collection#deleteMany": () => [{ a: 98 }],
    "docdb.Collection#findOneAndUpdate": () => [{ a: 1 }, { $set: { c: 1 } }, { returnDocument: "after", upsert: false }],
    "docdb.Collection#findOneAndReplace": () => [{ a: 1 }, { a: 1 }, { returnDocument: "before" }],
    "docdb.Collection#findOneAndDelete": () => [{ a: 97 }, { sort: { a: 1 } }],
    "docdb.Collection#bulkWrite": () => [[{ insertOne: { document: { a: 5 } } }, { deleteOne: { filter: { a: 5 } } }], { ordered: true }],
    "docdb.Collection#createIndex": () => [{ b: 1 }, { name: "b_1", unique: false, sparse: false }],
    "docdb.Collection#dropIndex": () => ["a_1"],
    "docdb.Collection#explain": () => [{ a: 1 }, { sort: { a: 1 } }],
    "docdb.Collection#watch": () => [[{ $match: { operationType: "insert" } }], { fullDocument: "updateLookup" }],
    "docdb.Database#watch": () => [[{ $project: { ns: 1 } }], { fullDocument: "default" }],
    "docdb.ChangeStream#instance": (c, s, ns) => {
        const coll = docdbColl(ns), cs = coll.watch();
        coll.insertMany(Array.from({ length: 300 }, (_, i) => ({ i })));
        return cs;
    },
    "docdb.Collection#aggregate": () => [[{ $match: { a: { $gte: 1 } } }, { $group: { _id: "$a", n: { $sum: 1 } } }, { $sort: { _id: 1 } }]],
    "docdb.Collection#persist": (c, s) => [s + "/docdb-coll.dds", { fsync: false }],
    "docdb.Database#new": () => ["fuzz", { changeStreamCapacity: 16 }],
    "docdb.Database#collection": () => ["c"],
    "docdb.Database#createCollection": () => [`n${++uniq}`, { changeStreamCapacity: 8 }],
    "docdb.Database#dropCollection": () => ["c"],
    "docdb.Database#persist": (c, s) => [s + "/docdb-db.dds", { fsync: false }],
    "docdb.Database#load": (c, s) => [docdbSnap(s), { merge: true }],
    "docdb.Database.load": (c, s) => [docdbSnap(s)],
    "vec.VectorIndex.load": (c, s, ns) => { const p = new c.Path(s + "/load.vec"); new ns.VectorIndex({ dims: 4 }).save(p, { fsync: false }); return [p]; },
    "docdb.ObjectId#new": () => ["507f1f77bcf86cd799439011"],
    "docdb.DocDBError#new": () => ["m", 2],
    "csv.CSVFile#new": (c, s, ns) => [new c.Path(freshFile(s, "a,b\n1,2\n3,4\n"))],
    "net.RateLimiter#new": () => [{ tokensPerSec: 10, burst: 5 }],
    ...fetchBaselines("net"), ...fetchBaselines("http"),
    "metrics.Metrics.gauge": () => ["sigfuzz_gauge", 3, { k: "v" }],
    "metrics.Metrics.histogram": () => ["sigfuzz_seconds", 0.2, { k: "v" }],
    "mathx.stats.cov": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.corr": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.ttestInd": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.ttestRel": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.mannWhitneyU": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.ks2Samp": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.pearson": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.spearman": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.psi": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.psiCounts": () => [[1, 2, 4, 8, 3], [2, 1, 5, 9, 4]],
    "mathx.stats.chi2Contingency": () => [[[10, 20], [30, 25]]],
    "mathx.signal.irfft": () => [[1, 2, 3], [0, 1, 0]],
    "mathx.signal.bls": () => { const t = Float64Array.from({ length: 40 }, (_, i) => i * 0.5); return [t, Float64Array.from(t, (x) => (x % 3 < 0.4 ? 0.99 : 1)), [2, 3], [0.3, 0.4]]; },
    "mathx.signal.kaiserOrd": () => [60, 0.1],
    "mathx.signal.firwin": () => [11, 0.3],
    "mathx.signal.lfilter": () => [[1, 2], [1, -0.5], [1, 2, 3, 4]],
    "mathx.signal.sosfilt": () => [[1, 0, 0, 1, -0.5, 0], [1, 2, 3, 4]],
    "mathx.signal.sosfreqz": () => [[1, 0, 0, 1, -0.5, 0]],
    "mathx.signal.convolve": () => [[1, 2, 3], [1, -1]],
    "mathx.signal.correlate": () => [[1, 2, 3], [1, -1]],
    "mathx.geo.haversine": () => [51.5, -0.12, 40.7, -74],
    "mathx.geo.haversineWithin": () => [[51.5, 40.7], [-0.12, -74], { lat: 51.5, lon: -0.1 }, 5000],
    "mathx.geo.pointInPolygon": () => [0.5, 0.5, [0, 0, 1, 1], [0, 1, 1, 0]],
    "mathx.geo.polygonArea": () => [[0, 0, 1, 1], [0, 1, 1, 0]],
    "mathx.Expression#eval": () => [{ x: 3, y: 0.5 }],
    "mathx.NDArray#new": () => [new Float64Array([1, 2, 3, 4, 5, 6]), [6]],
    "mathx.NDArray#set": () => [0, 1],
    "mathx.NDArray#broadcastTo": () => [[2, 6]],
    "mathx.solveSPD": () => [Float64Array.of(4, 2, 2, 3), Float64Array.of(2, 1)],
    "simd.gather": () => [new Float32Array(3), Float32Array.of(1, 2, 3, 4), Int32Array.of(3, 0, 1)],
    "simd.f64Gather": () => [new Float64Array(3), Float64Array.of(1, 2, 3, 4), Int32Array.of(3, 0, 1)],
    "simd.scatterAdd": () => [new Float32Array(4), Int32Array.of(3, 0, 3), Float32Array.of(1, 2, 3)],
    "simd.f64ScatterAdd": () => [new Float64Array(4), Int32Array.of(3, 0, 3), Float64Array.of(1, 2, 3)],
    "simd.gatherAdd": () => [new Float32Array(3), Float32Array.of(1, 2, 3, 4), Int32Array.of(3, 0, 1)],
    "simd.f64GatherAdd": () => [new Float64Array(3), Float64Array.of(1, 2, 3, 4), Int32Array.of(3, 0, 1)],
    "simd.spmv": () => [new Float32Array(2), Float32Array.of(1, 2, 3), Int32Array.of(0, 2, 1), Int32Array.of(0, 2, 3), Float32Array.of(1, 1, 1)],
    "simd.f64Spmv": () => [new Float64Array(2), Float64Array.of(1, 2, 3), Int32Array.of(0, 2, 1), Int32Array.of(0, 2, 3), Float64Array.of(1, 1, 1)],
    "structures.Graph#instance": (c, s, ns) => new ns.Graph({ directed: true, weighted: true }).addEdge(0, 1, 1).addEdge(1, 2, 2).addEdge(0, 2, 5),
    "structures.Graph#mst@this": (c, s, ns) => new ns.Graph({ weighted: true }).addEdge(0, 1, 1).addEdge(1, 2, 2).addEdge(0, 2, 5),
    "structures.Graph#modularity": () => [Int32Array.of(0, 0, 1)],
    "structures.Graph#maxFlow": () => [0, 2],
    "structures.Graph.fromEdges": () => [Int32Array.of(0, 1), Int32Array.of(1, 2), { types: Uint8Array.of(1, 2) }],
    "structures.Graph#topicPageRank": () => [[[0], { seeds: [1], weights: [2] }], { damping: 0.85 }],
    "structures.Graph#pathEndpoints": () => [0, [-1]],
    "structures.Graph#pathsInto": () => [0, [-1], new Int32Array(8), new Uint8Array(4)],
    "structures.Graph#bipartiteMatching@this": (c, s, ns) => new ns.Graph().addEdge(0, 1).addEdge(1, 2).addEdge(2, 3),
    ...graphCsrBaselines(),
    "structures.UnionFind#new": () => [4],
    "structures.PatriciaTrie#addCIDR": () => ["10.0.0.0/8", 1],
    "structures.PatriciaTrie#addCIDR@this": (c, s, ns) => new ns.PatriciaTrie(),
    "structures.PatriciaTrie#matchIP": () => ["10.1.2.3"],
    "structures.PatriciaTrie#matchIP@this": (c, s, ns) => new ns.PatriciaTrie().addCIDR("10.0.0.0/8", 1),
    "structures.PatriciaTrie#deleteCIDR": () => ["10.0.0.0/8"],
    "structures.PatriciaTrie#deleteCIDR@this": (c, s, ns) => new ns.PatriciaTrie().addCIDR("10.0.0.0/8", 1),
    "structures.CSRGraph#new": () => [3, [[0, 1, 1], [1, 2, 2], [0, 2, 5]]],
    "structures.RStarTree#insert": () => [[0, 0], [1, 1], 1],
    "structures.RStarTree#remove": () => [[0, 0], [1, 1], 1],
    "structures.RStarTree#search": () => [[0, 0], [2, 2]],
    "structures.RStarTree#nearest": () => [[0, 0], 2],
    "structures.KDTree#new": () => [[[0, 0], [1, 2], [3, 1], [2, 2]]],
    "structures.KDTree#box": () => [[0, 0], [2, 2]],
    "structures.BKDTree#new": () => [[[0, 0], [1, 2], [3, 1], [2, 2]]],
    "structures.BKDTree.build": () => [[[0, 0], [1, 2], [3, 1], [2, 2]]],
    "structures.BKDTree#range": () => [[0, 0], [2, 2]],
    "structures.BKDTree#rangeCount": () => [[0, 0], [2, 2]],
    "structures.BKDTree#cidr": () => ["10.0.0.0/8"],
    "structures.BKDTree#box": () => [-10, 10, -20, 20],
    "structures.BKDTree#distance": () => [0, 0, 1000],
    "structures.IndexedHeap#pushMany": () => [[1, 2, 3], [0.5, 2, 1]],
    "compress.Compressor#decompress": async (c) => [(await c.fresh("Compressor")).compress("hello, hello, hello")],
    "csv.CSVFile#updateCell": () => [{ row: 0, column: "a", value: "v" }],
    "csv.CSVFile#create@this": (c, s, ns) => new ns.CSVFile(new c.Path(`${s}/new${++uniq}.csv`)),
    "structures.CountMinSketch#merge": async (c) => [await c.fresh("CountMinSketch")],
    "time.Duration#new": () => [{ days: 1, hours: 2 }],
    "time.RRule#new": () => [{ freq: "DAILY", count: 3, dtstart: "2024-01-01T00:00:00Z" }],
    "ml.Pipeline#new": (c, s, ns) => [[new ns.StandardScaler(), new ns.LogisticRegression()]],
    "time.parseDate": () => ["2024-02-29"],
    "time.parseTime": () => ["12:34:56.789"],
    "time.Format#parse": () => ["2024-02-29"],
    "xml.XmlWriter#instance": (c, s, ns) => { const w = new ns.XmlWriter(); w.open("a"); w.open("b"); return w; },
    "xml.SAXParser#instance": (c, s, ns) => { const p = new ns.SAXParser(); p.write("<a k='v'>t<b/><!-- c --></a>"); return p; },
    "url.URL#new": () => ["https://user@example.com:8080/a/b?x=1#f", "https://example.org/"],
    "url.URL.join": () => ["https://example.com/a/b", "../c?d=1"],
    "oauth2.generateCodeChallenge": () => [PKCE_VERIFIER, "S256"],
    "oauth2.verifyCodeChallenge": (c, s, ns) => [PKCE_VERIFIER, ns.generateCodeChallenge(PKCE_VERIFIER, "S256"), "S256"],
    "oauth2.verifyJWT": async () => [(await cryptoNs()).JWTSign({ sub: "a", aud: "app", iss: "me", scope: "read write", exp: 4102444800 }, "k", { alg: "HS256" }), "k",
        { algorithms: ["HS256"], aud: "app", iss: "me", requiredScope: ["read"] }],
    "oauth2.buildAuthorizationUrl": (c, s, ns) => [{ authorizationEndpoint: "https://auth.example.com/authorize", clientId: "client", redirectUri: "https://app.example.com/cb",
        scope: "read", state: "st", codeChallenge: ns.generateCodeChallenge(PKCE_VERIFIER, "S256"), codeChallengeMethod: "S256" }],
    "oauth2.parseTokenResponse": () => ['{"access_token":"tok","token_type":"Bearer","expires_in":3600,"refresh_token":"r","scope":"read"}'],
    ...cryptoBaselines("crypto"),
    "encoding.Base58CheckDecode": (c, s, ns) => [ns.Base58CheckEncode(u8(1, 2, 3))],
    "encoding.base58CheckDecodeInto": (c, s, ns) => [ns.Base58CheckEncode(u8(1, 2, 3)), new Uint8Array(64)],
    "crypto.HMAC": () => ["sha256", "key", "data"],
    "crypto.PBKDF2": () => [{ hash: "sha256", password: "pw", salt: "salt0000", iterations: 2, length: 16 }],
    "crypto.JWTVerify": (c, s, ns) => [ns.JWTSign({ sub: "a" }, "k", { alg: "HS256" }), "k", { algorithms: ["HS256"] }],
    "async.withTimeout": () => [Promise.resolve(1), 50],
    "async.offload": () => [Math.abs, -1],
    "decimal.Money.fromDecimal": (c, s, ns) => [new ns.Decimal("12.34"), "USD"],
    "time.RRule.fromString": () => ["FREQ=DAILY;COUNT=3"],
    "bytes.Bytes#new": () => [new Uint8Array(64).fill(1)],
    "time.PlainDate#new": () => [2024, 2, 29],
    "compress.Dictionary#new": () => [["hello", "world"]],
    "crypto.AESGCM#new": () => [new Uint8Array(32).fill(3)],
    "crypto.ChaCha20Poly1305#new": () => [new Uint8Array(32).fill(3)],
    "crypto.Hmac#new": () => ["sha256", "key"],
    ...Object.fromEntries(["SUM_CHECKED", "BITWISE_AND", "BITWISE_OR", "BITWISE_XOR", "GROUP_BITMAP"].map((v) => [`dataframe.DataFrame#${v}`, () => ["k"]])),
    ...Object.fromEntries(["GROUP_BIT_AND", "GROUP_BIT_OR", "GROUP_BIT_XOR"].map((v) => [`dataframe.DataFrame#${v}`, () => ["k", "p"]])),
    ...Object.fromEntries(["JSON_AGG_STRICT", "JSON_OBJECT_AGG_STRICT"].map((v) => [`dataframe.DataFrame#${v}`, () => ["k", "y"]])),
    "dataframe.DataFrame#DROP_NA": () => ["x", "y"],
    // a right-side string column is refused (no value for an unmatched row)
    "dataframe.DataFrame#ASOF_JOIN": (c, s, ns) => [new ns.DataFrame({ v: new Float64Array([9, 8, 7]), o: new Int32Array([1, 3, 5]) }), "o", "o"],
    "ml.CSR#new": () => [[1, 2, 3], [0, 1, 0], [0, 2, 3], 2],
    "ml.crossValScore": (c, s, ns) => [() => new ns.LinearRegression(), [[0], [1], [2], [3], [4], [5]], [0, 2, 4, 6, 8, 10], { k: 2 }],
    "ml.gridSearch": (c, s, ns) => [(p) => new ns.LogisticRegression(p), [[0], [1], [2], [3], [4], [5]], [0, 0, 0, 1, 1, 1], { C: [0.1, 1] }, { k: 2 }],
    "ml.randomSearch": (c, s, ns) => [(p) => new ns.LogisticRegression(p), [[0], [1], [2], [3], [4], [5]], [0, 0, 0, 1, 1, 1], { C: [0.1, 1] }, { k: 2, nIter: 2 }],
    // {out: name} creates a column when the name is new, so the shared receiver grows columns as the sweep
    // runs; CONCAT needs two frames with the same columns, so it gets a fresh pair of its own.
    "dataframe.DataFrame#CONCAT@this": (c, s, ns) => BASELINES["dataframe.DataFrame#instance"](c, s, ns),
    "dataframe.DataFrame#CONCAT": (c, s, ns) => [BASELINES["dataframe.DataFrame#instance"](c, s, ns)],
    "dataframe.DataFrame#instance": (c, s, ns) => new ns.DataFrame({
        x: new Float64Array([1, 2, NaN, 4, 5, -6, 7, 8]),
        y: new Float64Array([2, 4, 6, 8, 10, 12, 14, 16]),
        w: new Float64Array([1, 1, 2, 2, 1, 0, 1, 1]),
        t: new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]),
        p: new Int32Array([0, 1, 2, 3, 0, 1, 2, 3]),
        k: new Int32Array([0, 1, 0, 2, 1, 0, 2, 1]),
        o: new Int32Array([0, 1, 2, 3, 4, 5, 6, 7]),
        s: ["a", "b", "a", "c", "b", "a", "c", "b"],
    }),
};

// Generic hooks, consulted when no "module.Class#instance" / "module.Label"
// entry exists. dyna:ml: every class with a fit() is fitted on a small
// separable problem before its methods run (an unfitted model refuses
// predict/transform at the boundary and proves nothing about them), and its
// static deserialize/load get bytes and a file that model produced.
const MLX = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 2], [2, 3], [3, 2], [3, 3]];
const MLY = [0, 0, 0, 0, 1, 1, 1, 1];
export const INSTANCE = {
    ml: async (name, ctx, ns) => {
        const C = ns[name];
        if (typeof C !== "function" || !C.prototype || typeof C.prototype.fit !== "function") return undefined;
        const given = BASELINES["ml." + name + "#new"];
        const m = new C(...(given ? given(ctx, ctx.scratch, ns) : []));
        try { await m.fit(MLX, MLY); } catch (e) { try { await m.fit(MLX); } catch (e2) {} }
        return m;
    },
};
const FORM_INIT = { headers: { "content-type": "application/x-www-form-urlencoded" } };
const bodyReceiver = (cname, mname) => (!/^(Request|Response)$/.test(cname) ? undefined
    : mname === "formData" ? (c, s, ns) => (cname === "Request" ? new ns.Request("http://example.com/p", { method: "POST", body: "a=1&b=%C3%A9", ...FORM_INIT }) : new ns.Response("a=1&b=%C3%A9", FORM_INIT))
    : /^(json|text|bytes|arrayBuffer|blob|clone)$/.test(mname) ? (c) => c.fresh(cname) : undefined);
// A receiver in a particular state, by module: (cname, method) -> a receiver
// factory, or undefined for the shared instance. dyna:structures serializes a
// fresh instance: the shared one holds whatever hostile values earlier calls
// stored in it, which serialize and save rightly refuse.
export const THIS = {
    // parse runs on the baseline Command: earlier argument()/option() edge cases
    // register required arguments that parse then rightly demands
    cli: (cname, mname) => (cname === "Command" && mname === "parse" ? (c) => c.fresh(cname) : undefined),
    // a body is read once: every reader gets a Request/Response of its own; formData() needs a form body
    // (the JSON baseline body is rightly refused by it), so it reads a fresh urlencoded one
    net: (cname, mname) => bodyReceiver(cname, mname),
    http: (cname, mname) => bodyReceiver(cname, mname),
    // and a Graph query runs on the baseline graph: addEdge's edge cases leave
    // negative and non-finite weights the shortest-path algorithms refuse
    structures: (cname, mname) => (cname === "BKDTree" && mname === "cidr" ? (c) => new c.ns.BKDTree(["10.1.2.3", "::1", "192.168.0.1"], { type: "ip" })
        : cname === "BKDTree" && (mname === "box" || mname === "distance") ? (c) => new c.ns.BKDTree([[1, 2], [-5, 10], [0, 0]], { type: "latlon" })
        : mname === "serialize" || mname === "save" || (cname === "Graph" && !/^(add|remove)/.test(mname)) ? (c) => c.fresh(cname) : undefined),
    // every raylib receiver is made under the window that is open now (a hostile closed the last one)
    raylib: (cname) => (c) => rlReceiver(c.ns, cname, c.scratch),
    // get() reads a recording stream; the shared AudioStream plays
    sdl: (cname, mname) => (cname === "AudioStream" && mname === "get" ? (c) => new c.ns.AudioStream({ recording: true, channels: 1 }) : undefined),
};
export const FIXED = {
    // deserialize gets a record and load a file that a fresh instance wrote
    structures: async (label, ctx, scratch) => {
        const m = /^(\w+)\.(deserialize|load)$/.exec(label);
        if (!m) return undefined;
        const inst = await ctx.fresh(m[1]);
        if (!inst) return undefined;
        if (m[2] === "deserialize") return [inst.serialize()];
        if (!ctx.Path) return undefined;
        const p = new ctx.Path(`${scratch}/${m[1]}${++uniq}.dpf`);
        try { inst.save(p); } catch (e) { return undefined; }
        return [p];
    },
    ml: async (label, ctx, scratch, ns) => {
        const m = /^(\w+)\.(deserialize|load)$/.exec(label);
        if (!m) return undefined;
        const inst = await ctx.prepare(m[1]);
        if (!inst) return undefined;
        if (m[2] === "deserialize") return [inst.serialize()];
        if (!ctx.Path) return undefined;
        const p = new ctx.Path(scratch + "/" + m[1] + ".model");
        await inst.save(p);
        return [p];
    },
};

// dyna:structures `graph.*` take the exportCSR record; each call gets a fresh one (a weighted DAG, so
// topologicalLevels has an answer) because hostile coercions may mutate its typed arrays.
function graphCsrBaselines() {
    const csr = (ns) => new ns.Graph({ directed: true, weighted: true }).addEdge(0, 1, 1).addEdge(1, 2, 2).addEdge(0, 2, 5).exportCSR();
    const extra = { modularity: () => [Int32Array.of(0, 0, 1)], maxFlow: () => [0, 2], linkPrediction: () => [[[0, 1], [1, 2]]] };
    const out = {};
    for (const name of ["pageRank", "labelPropagation", "louvain", "modularity", "coreNumbers", "triangles", "clustering", "betweenness", "closeness",
        "stronglyConnectedComponents", "weaklyConnectedComponents", "topologicalLevels", "maxFlow", "linkPrediction"])
        out[`structures.graph.${name}`] = (c, s, ns) => [csr(ns), ...(extra[name]?.() ?? [])];
    out["structures.graph.bipartiteMatching"] = (c, s, ns) => [new ns.Graph().addEdge(0, 1).addEdge(1, 2).addEdge(2, 3).exportCSR()];
    return out;
}
