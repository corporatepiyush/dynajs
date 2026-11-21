// Live dashboard — streaming charts, KPI cards, a virtualised host table and floating windows on dyna:ui.
// apps:needs dyna:ui
// flags: --std
//
// WHAT IT IS
//   An operations dashboard whose data changes every frame: six metric series stream into line plots
//   (three in cards, three in draggable floating windows), a bar chart of load, KPI cards with
//   progress, and a 400-row host table. With no arguments it feeds itself and checks the properties a
//   live dashboard depends on: every new sample changes the frame, a paused feed costs no layout work,
//   a plot's ink spans its height for full-range data and collapses for a constant series, the table
//   asks for the cells of visible rows only, and a window dragged by its title moves exactly that far.
//
// MODULES  dyna:ui (Context, plot, bars, table, window, stats, pixelsInto) · dyna:sdl (only with --window)
//
// RUN      dynajs --std examples/ui/live-dashboard.js             feeds itself and checks it
//          dynajs --std examples/ui/live-dashboard.js --window    opens it in a window (needs CONFIG_SDL3)

import * as ui from "dyna:ui";

const SAMPLES = 240,
    ROWS = 400,
    VISIBLE = 8;
const series = Array.from({ length: 6 }, () => new Float64Array(SAMPLES));
const feed = { tick: 0, constant: false, cellCalls: 0 };
const rects = {};

function push() {
    feed.tick++;
    for (let s = 0; s < series.length; s++) {
        const a = series[s];
        a.copyWithin(0, 1);
        a[SAMPLES - 1] = feed.constant ? 1 : Math.sin((feed.tick + s * 17) / 9) + Math.sin(feed.tick / 3.7 + s) * 0.3;
    }
}

function app(u) {
    const t = feed.tick;
    u.menubar(() =>
        u.menu("Dashboard", () => {
            u.menuItem("Refresh");
            u.menuItem("Export");
        }),
    );
    u.row(() => {
        for (let k = 0; k < 4; k++)
            u.card("KPI " + k, { width: 150 }, () => {
                u.heading(String((t * (k + 3)) % 997));
                u.progress(((t + k * 25) % 100) / 100);
            });
    });
    u.wrap(() => {
        for (let s = 0; s < 3; s++)
            u.card("Series " + s, { width: 300 }, () => {
                u.plot("p" + s, series[s], { height: 70 });
                rects["plot" + s] = u.lastRect();
            });
    });
    u.bars(
        "load",
        Array.from({ length: 24 }, (_, i) => (Math.sin((t + i) / 5) + 1.2) * 10),
        { height: 60 },
    );
    u.table("Hosts", {
        columns: [
            { name: "Host", width: 120 },
            { name: "CPU", width: 60 },
            { name: "Mem", width: 60 },
        ],
        rows: ROWS,
        visibleRows: VISIBLE,
        cell: (r, c) => {
            feed.cellCalls++;
            return c === 0 ? "host-" + r : c === 1 ? ((r * 7 + t) % 100) + "%" : ((r * 13) % 64) + " GB";
        },
    });
    for (let w = 0; w < 3; w++) {
        u.window("Live " + w, { x: 700 + w * 30, y: 80 + w * 60, width: 360, height: 180 }, () => {
            u.plot("w" + w, series[3 + w], { height: 90 });
            u.row(() => {
                u.spinner();
                u.label("streaming");
            });
        });
        rects["win" + w] = u.lastRect();
    }
    u.statusbar(() => u.label("tick " + t));
}

const W = 1280,
    H = 800;
const c = new ui.Context({ width: W, height: H, clock: "manual" });
let now = 0;
const frame = (advance = true) => {
    if (advance) {
        now += 1 / 60;
        c.setTime(now);
    }
    return c.frame(app);
};

// The vertical extent, in points, of what a plot drew inside its padding (border + 2 points): any
// pixel that is not the plot's own background, anti-aliased edges included.
function inkSpan(r) {
    const px = c.pixelsInto(buf),
        s = c.scale,
        inset = c.theme().borderWidth + 3;
    const x0 = Math.ceil((r.x + inset) * s),
        x1 = Math.floor((r.x + r.width - inset) * s);
    const y0 = Math.ceil((r.y + inset) * s),
        y1 = Math.floor((r.y + r.height - inset) * s);
    const bg = c.theme().frame >>> 8;
    let top = Infinity,
        bottom = -Infinity;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const i = (y * c.pixelWidth + x) * 4;
            if (((px[i] << 16) | (px[i + 1] << 8) | px[i + 2]) !== bg) {
                top = Math.min(top, y);
                bottom = Math.max(bottom, y);
            }
        }
    return bottom >= top ? (bottom - top + 1) / s : 0;
}
let buf = null;

const results = {};
if (scriptArgs.includes("--window")) await runWindow();
else {
    for (let k = 0; k < SAMPLES; k++) push();
    frame();
    frame();
    buf = new Uint8Array(c.pixelWidth * c.pixelHeight * 4);

    let changed = 0;
    for (let k = 0; k < 60; k++) {
        push();
        if (frame()) changed++;
    }
    results["every new sample changes the frame"] = changed === 60;

    const before = c.stats().reusedFrames;
    let still = 0;
    for (let k = 0; k < 30; k++) if (!frame(false)) still++;
    results["a paused feed redraws nothing: 30 frames reused, none changed"] =
        still === 30 && c.stats().reusedFrames - before === 30;

    feed.cellCalls = 0;
    frame();
    results["the table asks for the visible rows' cells only (8 of 400 rows x 3 columns)"] =
        feed.cellCalls > 0 && feed.cellCalls <= (VISIBLE + 1) * 3;

    const r = rects.plot0,
        full = inkSpan(r);
    results["a plot's line spans most of its height for data covering its range"] = full >= 0.8 * r.height;
    feed.constant = true;
    for (let k = 0; k < SAMPLES; k++) push();
    frame();
    const flat = inkSpan(rects.plot0);
    results["and collapses to a thin line for a constant series"] = flat > 0 && flat <= 0.15 * r.height;
    feed.constant = false;

    const w = rects.win1,
        x0 = w.x + 60,
        y0 = w.y + 8;
    c.pointer({ x: x0, y: y0, buttons: 1 });
    frame();
    for (let k = 1; k <= 5; k++) {
        c.pointer({ x: x0 - 20 * k, y: y0 + 10 * k, buttons: 1 });
        frame();
    }
    c.pointer({ x: x0 - 100, y: y0 + 50, buttons: 0 });
    frame();
    frame();
    const moved = rects.win1;
    results["dragging a window's title bar moves it by exactly the drag (-100, +50)"] =
        moved.x === w.x - 100 && moved.y === w.y + 50 && moved.width === w.width;

    c.close();
    check(results);
}

async function runWindow() {
    let sdl;
    try {
        sdl = await import("dyna:sdl");
    } catch (e) {
        throw new Error("--window needs dyna:sdl (build with CONFIG_SDL3=y)");
    }
    sdl.init(["video"]);
    const win = new sdl.Window("Dashboard", W, H, { resizable: true, highDensity: true });
    const r = new sdl.Renderer(win);
    const w = new ui.Context({ target: r });
    for (let k = 0; k < SAMPLES; k++) push();
    for (let running = true; running;) {
        for (const e of sdl.pollEvents()) if (e.type === "quit") running = false;
        push();
        w.frame(app);
        w.idle(1 / 30);
    }
    w.close();
    r.close();
    win.close();
    sdl.quit();
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
