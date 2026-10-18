// flags: --std
// dyna:ui on its GPU backends, compared over the WHOLE screen with the in-memory rasteriser that
// every other suite pins: a frame drawn through dyna:sdl or dyna:raylib must equal Context.pixels()
// of an identical target-less Context, except on anti-aliased edges, where each renderer
// interpolates fringe alpha its own way. "Edge" is defined from the reference frame alone: a pixel
// one of whose 8 neighbours has another colour there. Square geometry has no fringes, so that scene
// must match exactly. The gate's ui stage builds this binary with SDL3/raylib when they are
// installed and runs it on SDL's dummy drivers; a backend that is not in the binary says SKIP.
import * as ui from "dyna:ui";

let pass = 0, fail = 0, skipped = [];
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function refuses(fn, re, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    ok(e !== null && re.test(String(e.message)), `${m} (got ${e})`);
}

const square = (u) => { u.label("Label"); u.button("Button"); u.checkbox("Check", true); u.slider("S", 0.4, 0, 1); u.progress(0.6); u.separator(); };
const scene = (u) => { square(u); u.radio("R", ["a", "b"], 1, { horizontal: true }); u.switch("Sw", true); };

function edges(ref, w, h) {
    const px = (x, y) => ((ref[(y * w + x) * 4] << 24) | (ref[(y * w + x) * 4 + 1] << 16) | (ref[(y * w + x) * 4 + 2] << 8) | ref[(y * w + x) * 4 + 3]) >>> 0;
    const e = new Uint8Array(w * h);
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const c = px(x, y);
            for (let dy = -1; dy <= 1 && !e[y * w + x]; dy++)
                for (let dx = -1; dx <= 1; dx++) {
                    const nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h && px(nx, ny) !== c) { e[y * w + x] = 1; break; }
                }
        }
    return e;
}

// got/ref RGBA; interior pixels must be within `tol` per channel, edge pixels may differ freely.
function compare(got, ref, w, h, tol) {
    const e = edges(ref, w, h);
    let exact = 0, worstInterior = 0, at = -1, edgeDiffs = 0;
    for (let i = 0; i < w * h; i++) {
        let d = 0;
        for (let c = 0; c < 4; c++) d = Math.max(d, Math.abs(got[i * 4 + c] - ref[i * 4 + c]));
        if (!d) exact++;
        else if (e[i]) edgeDiffs++;
        else if (d > worstInterior) { worstInterior = d; at = i; }
    }
    return { exact, total: w * h, worstInterior, at: at < 0 ? null : [at % w, Math.floor(at / w)], edgeDiffs, interiorOk: worstInterior <= tol };
}

let sdl = null;
try { sdl = await import("dyna:sdl"); } catch (e) { skipped.push("sdl (dyna:sdl is not in this binary)"); }
if (sdl) {
    sdl.init(["video"]);
    const win = new sdl.Window("ui-backend", 240, 160, { hidden: true });
    const r = new sdl.Renderer(win);
    const a = new ui.Context({ target: r, present: false, clock: "manual" });
    ok(a.pixelWidth === r.outputSize.width && a.pixelHeight === r.outputSize.height && a.scale === win.displayScale,
        `a renderer-backed Context takes its pixel size and scale from the target before any frame (${a.pixelWidth}x${a.pixelHeight} at ${a.scale})`);
    ok(a.insets.join() === "0,0,0,0", `a desktop window has no safe-area insets (${a.insets})`);
    const b = new ui.Context({ width: a.width, height: a.height, scale: a.scale, clock: "manual" });
    a.frame(square); b.frame(square);
    let g = r.readPixels(), res = compare(g.pixels, b.pixels(), g.width, g.height, 0);
    ok(res.exact === res.total, `square geometry through the SDL renderer (${r.name}) equals the in-memory frame on every pixel (${res.exact}/${res.total})`);
    a.theme({ radius: 6 }); b.theme({ radius: 6 });
    a.frame(scene); b.frame(scene);
    g = r.readPixels();
    res = compare(g.pixels, b.pixels(), g.width, g.height, 0);
    ok(res.interiorOk && res.exact > 0.9 * res.total,
        `rounded geometry: every interior pixel exact, differences only on anti-aliased edges (${res.exact}/${res.total} exact, ${res.edgeDiffs} edge pixels differ, worst interior ${res.worstInterior} at ${res.at})`);

    {
        const blob = (u) => u.box({ float: "screen", x: 50, y: 40, width: 41, height: 23, radius: 7, fill: 0x3d7effff }, () => {});
        a.theme({ radius: 0 }); b.theme({ radius: 0 });
        a.frame(blob); b.frame(blob);
        const got = r.readPixels(), ref = b.pixels(), bg = ref.slice(0, 4);
        const bbox = (px, w, h) => {
            let x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (let y = 0; y < h; y++)
                for (let x = 0; x < w; x++) {
                    const i = (y * w + x) * 4;
                    if (px[i] !== bg[0] || px[i + 1] !== bg[1] || px[i + 2] !== bg[2]) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
                }
            return [x0, y0, x1, y1].join();
        };
        // SDL's software geometry takes whole-pixel vertex positions (SDL_Point), so the half-pixel
        // anti-aliasing rim lands a pixel off on one side; GPU renderers keep it. Either way the
        // extent is within one pixel of the in-memory frame on every side, and never shifted whole.
        const gb = bbox(got.pixels, got.width, got.height).split(",").map(Number), rb = bbox(ref, got.width, got.height).split(",").map(Number);
        ok(gb.every((v, i) => Math.abs(v - rb[i]) <= 1) && gb[2] - gb[0] >= rb[2] - rb[0] - 1,
            `a lone rounded box covers the in-memory extent within one pixel per side through SDL (${r.name}: ${gb}, memory ${rb})`);
    }

    let name = "", focusedActive = false;
    const field = (u) => { name = u.textField("Name", name); };
    a.frame(field);
    a.pointer({ x: 20, y: 15, buttons: 1 }); a.frame(field);
    a.pointer({ x: 20, y: 15, buttons: 0 }); a.frame(field);
    focusedActive = win.textInputActive;
    ok(a.editing && focusedActive, `focusing a text field starts the window's text input (editing ${a.editing}, textInputActive ${focusedActive})`);
    a.pointer({ x: 200, y: 140, buttons: 1 }); a.frame(field);
    a.pointer({ x: 200, y: 140, buttons: 0 }); a.frame(field);
    ok(!a.editing && !win.textInputActive, "a press elsewhere stops it again");
    a.type("x");
    a.frame(field);
    ok(name === "", `typed text without focus goes nowhere (${JSON.stringify(name)})`);

    win.setSize(300, 200);
    sdl.pollEvents();
    a.frame(scene);
    ok(a.pixelWidth === r.outputSize.width && a.pixelWidth === 300 * a.scale, `after the window grows the next frame lays out at its new size (${a.pixelWidth})`);

    refuses(() => a.frame((u) => { a.close(); }), /while one of its methods is running/, "closing a Context inside its own frame is refused");
    r.close();
    refuses(() => a.frame(scene), /closed|renderer/i, "a frame on a closed renderer is refused, naming why");
    a.close(); b.close();
    ok(a.closed, "close() marks the Context closed");
    win.close();
    sdl.quit();
}

let rl = null;
try { rl = await import("dyna:raylib"); } catch (e) { skipped.push("raylib (dyna:raylib is not in this binary)"); }
if (rl) {
    let w = null;
    try { w = new rl.Window(240, 160, "ui-backend", { hidden: true }); } catch (e) { skipped.push(`raylib (no window: ${e.message})`); }
    if (w) {
        // raylib has no window readback, so the frame is drawn into a RenderTexture the window's size
        // through a { present: false } Context, which submits exactly what a presenting one does.
        rl.setLogLevel("none");
        const a = new ui.Context({ target: w, present: false, clock: "manual" });
        const b = new ui.Context({ width: a.width, height: a.height, scale: a.scale, clock: "manual" });
        ok(a.pixelWidth > 0 && a.pixelHeight > 0, `a raylib-backed Context takes its size from the window (${a.pixelWidth}x${a.pixelHeight} at ${a.scale})`);
        const rt = new rl.RenderTexture(a.pixelWidth, a.pixelHeight);
        refuses(() => a.frame(square), /inside rl\.frame\(\) or RenderTexture#draw\(\)/, "a { present: false } raylib Context outside any frame is refused");
        rt.draw(() => { rl.clearBackground(0); a.frame(square); });
        b.frame(square);
        let g = rt.readPixels(), res = compare(g.pixels, b.pixels(), g.width, g.height, 0);
        ok(res.exact === res.total, `square geometry through raylib equals the in-memory frame on every pixel: nothing is culled (${res.exact}/${res.total})`);
        a.theme({ radius: 6 }); b.theme({ radius: 6 });
        w.frame(() => rt.draw(() => { rl.clearBackground(0); a.frame(scene); }));
        b.frame(scene);
        g = rt.readPixels();
        res = compare(g.pixels, b.pixels(), g.width, g.height, 2);
        ok(res.interiorOk && res.exact > 0.9 * res.total,
            `raylib, rounded geometry inside rl.frame(): interiors within 2 per channel, differences on anti-aliased edges only (${res.exact}/${res.total} exact, worst interior ${res.worstInterior} at ${res.at})`);
        const red = () => { const p = rt.readPixels().pixels; let n = 0; for (let i = 0; i < p.length; i += 4) if (p[i] === 255 && p[i + 3] === 255) n++; return n; };
        rt.draw(() => { rl.clearBackground(0); a.frame(square); rl.drawTriangle(10, 10, 10, 30, 30, 10, 0xff0000ff); });
        const front = red();
        rt.draw(() => { rl.clearBackground(0); a.frame(square); rl.drawTriangle(10, 10, 30, 10, 10, 30, 0xff0000ff); });
        const back = red();
        ok(front > 100 && back === 0, `after the UI, raylib still culls a clockwise triangle and draws a counter-clockwise one: culling is restored (${front} vs ${back} pixels)`);
        // Inside RenderTexture#draw the Context lays out at the TEXTURE's size, at the window's density:
        // a half-height and a half-width texture each equal an in-memory Context of that logical size.
        a.theme({ radius: 0 });
        const small = (u) => { u.label("Label"); u.button("Button"); u.checkbox("Check", true); };
        for (const [tw, th] of [[a.pixelWidth, a.pixelHeight >> 1], [a.pixelWidth >> 1, a.pixelHeight]]) {
            const t = new rl.RenderTexture(tw, th);
            t.draw(() => { rl.clearBackground(0); a.frame(small); });
            const sc = a.scale, ref = new ui.Context({ width: tw / sc, height: th / sc, scale: sc, clock: "manual" });
            ref.theme({ radius: 0 });
            ref.frame(small);
            g = t.readPixels();
            res = compare(g.pixels, ref.pixels(), g.width, g.height, 0);
            ok(a.pixelWidth === tw && a.pixelHeight === th && res.exact === res.total,
                `a ${tw}x${th} RenderTexture lays the Context out at its own size and the window's density (${a.pixelWidth}x${a.pixelHeight} at ${a.scale}; ${res.exact}/${res.total} exact)`);
            ref.close(); t.close();
        }
        // The UI composites alpha as "over" while it draws, then hands raylib back its default blend:
        // a translucent rect over a cleared texture keeps raylib's own alpha (a*a) after a UI frame.
        const tiny = new rl.RenderTexture(8, 8);
        tiny.draw(() => { rl.clearBackground(0); rl.drawRectangle(0, 0, 8, 8, 0xff000080); });
        const alone = tiny.readPixels().pixels[3];
        tiny.draw(() => { rl.clearBackground(0); a.frame(square); rl.clearBackground(0); rl.drawRectangle(0, 0, 8, 8, 0xff000080); });
        const after = tiny.readPixels().pixels[3];
        ok(alone === after && alone < 0x80, `raylib's own blending is restored after a UI frame (alpha ${alone} alone, ${after} after the UI)`);
        tiny.close();
        rt.close();
        a.close(); b.close();
        w.close();
    }
}

for (const s of skipped) print("SKIP: " + s);
print(`test_ui_backends: ${pass} pass, ${fail} fail, ${skipped.length} skipped`);
if (fail) throw new Error(`${fail} failures`);
