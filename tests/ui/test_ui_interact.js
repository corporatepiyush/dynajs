// dyna:ui interactions: what a click, drag, key or typed text does to each control's VALUE, per the
// declarations in dynajs.d.ts. Widgets are located with lastRect() (the box laid out in the previous
// frame), so these cases do not restate layout; the layout itself is pinned by test_ui_widgets.js,
// test_ui_mobile.js and test_ui_scale.js. Input is one frame behind, as documented.
import * as ui from "dyna:ui";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }

const W = 320, H = 240;
function ctx() { return new ui.Context({ width: W, height: H, clock: "manual" }); }
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];

// Runs `scene` until the rects it records settle, then returns them by name.
function settle(c, scene) {
    let rects = {};
    for (let k = 0; k < 2; k++) { rects = {}; c.frame((u) => scene(u, (name) => { rects[name] = u.lastRect(); })); }
    return rects;
}
function click(c, scene, x, y, opts = {}) {
    c.pointer({ x, y, buttons: opts.buttons ?? 1, ...(opts.kind ? { kind: opts.kind } : {}) });
    c.frame((u) => scene(u, () => {}));
    c.pointer({ x, y, buttons: 0, ...(opts.kind ? { kind: opts.kind } : {}) });
    c.frame((u) => scene(u, () => {}));
}
function drag(c, scene, x0, y0, x1, y1, steps = 4) {
    c.pointer({ x: x0, y: y0, buttons: 1 }); c.frame((u) => scene(u, () => {}));
    for (let k = 1; k <= steps; k++) { c.pointer({ x: x0 + (x1 - x0) * k / steps, y: y0 + (y1 - y0) * k / steps, buttons: 1 }); c.frame((u) => scene(u, () => {})); }
    c.pointer({ x: x1, y: y1, buttons: 0 }); c.frame((u) => scene(u, () => {}));
}
function keys(c, scene, ...names) { for (const n of names) { c.key(n, true); c.frame((u) => scene(u, () => {})); c.key(n, false); c.frame((u) => scene(u, () => {})); } }

{
    const c = ctx();
    ok(c.frame((u) => { ok(u.lastRect() === null, "lastRect is null before any widget"); u.button("B"); ok(u.lastRect() === null, "and null for a widget not laid out yet (first frame)"); }) === true, "first frame changed");
    let r = null;
    c.frame((u) => { u.button("B"); r = u.lastRect(); });
    ok(r && r.x === 8 && r.y === 8 && r.width === 7 + 12 && r.height === 21, `lastRect is the previous frame's box in points (${JSON.stringify(r)})`);
    c.close();
}

{
    const c = ctx();
    let v = 0;
    const scene = (u, rec) => { v = u.slider("##s", v, 0, 9, { step: 1, decimals: 0 }); rec("s"); };
    const r = settle(c, scene).s;
    const x = (g) => r.x + 1 + 5 + g * (r.width - 2 - 10);
    click(c, scene, x(3 / 9), r.y + r.height / 2);
    ok(v === 3, `a click at 3/9 of the track's travel sets 3 (${v})`);
    drag(c, scene, x(3 / 9), r.y + 5, x(1), r.y + 5);
    ok(v === 9, `dragging to the end of the travel reaches the maximum (${v})`);
    drag(c, scene, x(1), r.y + 5, r.x - 50, r.y + 5);
    ok(v === 0, `dragging past the start clamps to the minimum (${v})`);
    keys(c, scene, "right", "right");
    ok(v === 2, `with focus, Right steps by the step (${v})`);
    keys(c, scene, "end");
    ok(v === 9, `End jumps to the maximum (${v})`);
    keys(c, scene, "home");
    ok(v === 0, `Home jumps to the minimum (${v})`);
    c.close();
}

{
    const c = ctx();
    let v = [2, 6];
    const scene = (u, rec) => { v = u.range("##r", v, 0, 10, { step: 1, decimals: 0 }); rec("r"); };
    const r = settle(c, scene).r;
    const before = v;
    c.frame((u) => scene(u, () => {}));
    ok(v === before, "range returns the same array while unchanged");
    drag(c, scene, r.x + r.width - 2, r.y + r.height / 2, r.x + r.width - 2, r.y + r.height / 2);
    ok(v[0] === 2 && v[1] === 10 && v !== before, `pressing near the high end moves the high thumb only ([${v}])`);
    drag(c, scene, r.x + 2, r.y + r.height / 2, r.x + 2, r.y + r.height / 2);
    ok(v[0] === 0 && v[1] === 10, `pressing near the low end moves the low thumb ([${v}])`);
    c.close();
}

{
    const c = ctx();
    let v = 5;
    const scene = (u, rec) => { v = u.drag("##d", v, { speed: 0.5, min: 0, max: 20 }); rec("d"); };
    const [x, y] = mid(settle(c, scene).d);
    drag(c, scene, x, y, x + 10, y);
    ok(v === 10, `dragging 10 points at speed 0.5 adds 5 (${v})`);
    drag(c, scene, x, y, x + 200, y);
    ok(v === 20, `and clamps to max (${v})`);
    c.close();
    const c2 = new ui.Context({ width: W, height: H, scale: 2, clock: "manual" });
    v = 5;
    const [x2, y2] = mid(settle(c2, scene).d);
    drag(c2, scene, x2, y2, x2 + 10, y2);
    ok(v === 10, `at scale 2 the same 10 points still add 5: speed is per point, not per pixel (${v})`);
    c2.close();
}

{
    const c = ctx();
    let v = 3;
    const scene = (u, rec) => { u.id("spin", (u) => { v = u.spin("##p", v, { step: 2, min: 0, max: 6 }); }); rec("p"); };
    const r = settle(c, scene).p;
    click(c, scene, r.x + 4, r.y + r.height / 2);
    ok(v === 1, `the left button steps down by 2 (${v})`);
    click(c, scene, r.x + 4, r.y + r.height / 2);
    ok(v === 0, `and clamps at min (${v})`);
    click(c, scene, r.x + r.width - 4, r.y + r.height / 2);
    click(c, scene, r.x + r.width - 4, r.y + r.height / 2);
    click(c, scene, r.x + r.width - 4, r.y + r.height / 2);
    click(c, scene, r.x + r.width - 4, r.y + r.height / 2);
    ok(v === 6, `the right button steps up and clamps at max (${v})`);
    c.close();
}

{
    const c = ctx();
    let s = "ab", submitted = false;
    const scene = (u, rec) => { s = u.textField("Name", s, { maxLength: 6 }); rec("f"); submitted = submitted || u.submitted; };
    const [x, y] = mid(settle(c, scene).f);
    click(c, scene, x, y);
    ok(c.editing, "a click focuses the field");
    keys(c, scene, "end");
    c.type("cd"); c.frame((u) => scene(u, () => {}));
    ok(s === "abcd", `typed text lands at the caret (${JSON.stringify(s)})`);
    c.type("xyz!"); c.frame((u) => scene(u, () => {}));
    ok(s === "abcdxy", `maxLength 6 refuses what does not fit (${JSON.stringify(s)})`);
    keys(c, scene, "backspace");
    ok(s === "abcdx", `Backspace deletes before the caret (${JSON.stringify(s)})`);
    keys(c, scene, "home");
    keys(c, scene, "delete");
    ok(s === "bcdx", `Home then Delete removes the first character (${JSON.stringify(s)})`);
    keys(c, scene, "return");
    ok(submitted, "Return reports submitted");
    c.close();
}

{
    const c = ctx();
    let s = "";
    const scene = (u, rec) => { s = u.textField("##p", s, { password: true }); rec("f"); };
    const [x, y] = mid(settle(c, scene).f);
    click(c, scene, x, y);
    c.type("hunter2"); c.frame((u) => scene(u, () => {}));
    ok(s === "hunter2", "a password field returns the real text");
    c.close();
}

{
    const c = ctx();
    let s = "fixed";
    const scene = (u, rec) => { s = u.textField("##ro", s, { readOnly: true }); rec("f"); };
    const [x, y] = mid(settle(c, scene).f);
    click(c, scene, x, y);
    c.type("zz"); c.frame((u) => scene(u, () => {}));
    keys(c, scene, "backspace");
    ok(s === "fixed", `a read-only field ignores typing and deletion (${JSON.stringify(s)})`);
    c.close();
}

{
    const c = ctx();
    let s = "one";
    const scene = (u, rec) => { s = u.textArea("##a", s, { rows: 3 }); rec("a"); };
    const [x, y] = mid(settle(c, scene).a);
    click(c, scene, x, y);
    keys(c, scene, "end");
    keys(c, scene, "return");
    c.type("two"); c.frame((u) => scene(u, () => {}));
    ok(s === "one\ntwo", `Return in a text area inserts a newline (${JSON.stringify(s)})`);
    c.close();
}

{
    const c = ctx();
    let s = "query";
    const scene = (u, rec) => { s = u.search("##q", s); rec("q"); };
    const r = settle(c, scene).q;
    click(c, scene, r.x + r.width + 2 + r.height / 2, r.y + r.height / 2);
    ok(s === "", `the clear button empties the search (${JSON.stringify(s)})`);
    c.close();
}

{
    const c = ctx();
    let v = 3;
    const scene = (u, rec) => { v = u.rating("##rt", v, { max: 5 }); rec("rt"); };
    const r = settle(c, scene).rt, star = r.width / 5;
    click(c, scene, r.x + star * 4.5, r.y + r.height / 2);
    ok(v === 5, `clicking the fifth star sets 5 (${v})`);
    click(c, scene, r.x + star * 4.5, r.y + r.height / 2);
    ok(v === 0, `clicking the current value again clears it (${v})`);
    c.close();
}

{
    const c = ctx();
    let p = 3, got = -1;
    const scene = (u, rec) => { const q = u.pagination("##pg", p, 10); if (q !== p) p = q; rec("pg"); };
    const r = settle(c, scene).pg;
    click(c, scene, r.x + r.width - 6, r.y + r.height / 2);
    ok(p === 4, `the last control of the pager goes to the next page (${p})`);
    click(c, scene, r.x + 6, r.y + r.height / 2);
    ok(p === 3, `the first goes to the previous page (${p})`);
    let crumb = -1;
    const bc = (u, rec) => { const k = u.breadcrumbs("##bc", ["Home", "Docs", "Page"]); if (k >= 0) crumb = k; rec("bc"); };
    const b = settle(c, bc).bc;
    click(c, bc, b.x + 10, b.y + b.height / 2);
    ok(crumb === 0, `clicking the first crumb reports 0 (${crumb})`);
    c.close();
}

{
    const c = ctx();
    let open = 0, toggled = false;
    const scene = (u, rec) => { const o = u.collapsing("Section", false); rec("c"); if (o) { open++; u.label("inside"); } };
    const r = settle(c, scene).c;
    ok(open === 0, "a collapsing header starts as given (closed)");
    click(c, scene, r.x + 10, r.y + r.height / 2);
    c.frame((u) => scene(u, () => {}));
    ok(open > 0, "a click opens it");
    const n = open;
    click(c, scene, r.x + 10, r.y + r.height / 2);
    c.frame((u) => scene(u, () => {}));
    ok(open === n + 1, `a second click closes it (opened in ${open - n} more frames)`);
    c.close();
}

{
    const c = ctx();
    let ratio = 0.5;
    const scene = (u, rec) => { u.column({ width: "grow", height: 100 }, () => { ratio = u.split("##sp", ratio, (u) => u.label("a"), (u) => u.label("b")); rec("sp"); }); };
    const r = settle(c, scene).sp;
    const hx = r.x + r.width * 0.5;
    drag(c, scene, hx + 1, r.y + 50, r.x + r.width * 0.25 + 2, r.y + 50);
    ok(Math.abs(ratio - 0.25) < 0.004 && r.width > 200, `dragging the handle's centre to a quarter of ${r.width} points moves the split there (${ratio.toFixed(4)})`);
    drag(c, scene, r.x + r.width * 0.25 + 2, r.y + 50, r.x - 100, r.y + 50);
    ok(Math.abs(ratio - 0.05) < 1e-6, `and it clamps at 0.05 (${ratio})`);
    c.close();
}

{
    const c = ctx();
    let shown = true;
    const scene = (u, rec) => { if (shown) shown = u.window("Win", { x: 40, y: 40, width: 140, height: 90, closable: true }, (u) => u.label("in")); rec("w"); };
    settle(c, scene);
    drag(c, scene, 60, 48, 110, 98);
    let r = null;
    c.frame((u) => { if (shown) shown = u.window("Win", { x: 40, y: 40, width: 140, height: 90, closable: true }, (u) => u.label("in")); r = u.lastRect(); });
    ok(r && r.x === 90 && r.y === 90 && r.width === 140, `dragging the title bar by (50, 50) moves the window by exactly that (${JSON.stringify(r)})`);
    let closeAt = null;
    c.frame((u) => { if (shown) shown = u.window("Win", { x: 40, y: 40, width: 140, height: 90, closable: true }, (u) => u.label("in")); closeAt = u.lastRect(); });
    click(c, scene, closeAt.x + closeAt.width - 8, closeAt.y + 8);
    ok(shown === false, "the close button makes window() return false");
    c.close();
}

{
    const c = ctx();
    let picked = 0, open = false;
    const scene = (u, rec) => { u.menubar(() => { open = u.menu("File", () => { if (u.menuItem("Open")) picked++; u.menuItem("Quit"); }); rec("m"); }); };
    const r = settle(c, scene).m;
    click(c, scene, r.x + r.width / 2, r.y + r.height / 2);
    ok(open, "a click on a menu title opens it");
    let item = null;
    c.frame((u) => u.menubar(() => { u.menu("File", () => { u.menuItem("Open"); item = u.lastRect(); u.menuItem("Quit"); }); }));
    click(c, scene, item.x + item.width / 2, item.y + item.height / 2);
    c.frame((u) => scene(u, () => {}));
    ok(picked === 1 && !open, `clicking an item runs it once and closes the menu (picked ${picked}, open ${open})`);
    c.close();
}

{
    const c = ctx();
    let d = { year: 2026, month: 10, day: 30 };
    const scene = (u, rec) => { d = u.datePicker("##dp", d); rec("dp"); };
    const r = settle(c, scene).dp;
    const before = d;
    c.frame((u) => scene(u, () => {}));
    ok(d === before, "datePicker returns the same object while unchanged");
    ok(d.year === 2026 && d.month === 10 && d.day === 30, "and keeps the date it was given");
    c.close();
}

// The date picker's grid, from its layout rules: header row (frame height 21), column gap 4,
// weekday row (line + padY = 17), then 26-point cells (2 lines), 7 per row, Monday first.
{
    const c = ctx();
    let d = { year: 2026, month: 10, day: 30 };
    const scene = (u, rec) => { d = u.datePicker("##dp", d); rec("dp"); };
    let r = settle(c, scene).dp;
    ok(r.width === 7 * 26 && r.height === 21 + 4 + 17 + 5 * 26, `October 2026 (3 leading cells, 5 rows) lays out as 7 x 26 by 42 + 5 x 26 (${r.width}x${r.height})`);
    const before = d;
    click(c, scene, r.x + r.width - 8, r.y + 10);
    ok(d === before, "moving to the next month does not change the selected date");
    r = settle(c, scene).dp;
    ok(r.height === 21 + 4 + 17 + 6 * 26, `November 2026 starts on a Sunday: 6 leading cells, 6 rows (${r.height})`);
    const idx = 6 + 15 - 1;
    click(c, scene, r.x + (idx % 7) * 26 + 13, r.y + 42 + Math.floor(idx / 7) * 26 + 13);
    ok(d !== before && d.year === 2026 && d.month === 11 && d.day === 15, `clicking the 15th of the shown month selects 2026-11-15 (${JSON.stringify(d)})`);
    click(c, scene, r.x + 8, r.y + 10);
    click(c, scene, r.x + 8, r.y + 10);
    r = settle(c, scene).dp;
    ok(r.height === 21 + 4 + 17 + 5 * 26, `two steps back is September 2026: starts on a Tuesday, 1 leading cell + 30 days = 5 rows (${r.height})`);
    c.close();
}

{
    const c = ctx();
    let res = null, sel = -1, sort = { column: -1, descending: false };
    const scene = (u, rec) => { res = u.table("##t", { columns: [{ name: "Name", width: 120 }, { name: "Size", width: 80 }], rows: 20, cell: (row, col) => `${row}:${col}`, visibleRows: 5, selected: sel, sort }); sel = res.selected; sort = { column: res.sortColumn, descending: res.descending }; rec("t"); };
    const r = settle(c, scene).t;
    click(c, scene, r.x + 120 + 20, r.y + 8);
    ok(res.sortColumn === 1 && !res.descending, `clicking a header sorts by that column ascending (${JSON.stringify(res)})`);
    click(c, scene, r.x + 120 + 20, r.y + 8);
    ok(res.sortColumn === 1 && res.descending, `clicking it again sorts descending (${JSON.stringify(res)})`);
    const rh = 17;
    click(c, scene, r.x + 30, r.y + 21 + rh * 2 + rh / 2);
    ok(sel === 2, `a mouse press on the third body row selects row 2 (${sel})`);
    c.close();
}

{
    const c = ctx();
    let open = false, clicks = 0;
    const scene = (u, rec) => { open = u.tree("Root", () => { u.tree("Leaf", { leaf: true }); }); rec("t"); };
    const r = settle(c, scene).t;
    ok(!open, "a tree node starts closed by default");
    click(c, scene, r.x + 10, r.y + r.height / 2);
    ok(open, "a click opens it");
    click(c, scene, r.x + 10, r.y + r.height / 2);
    ok(!open, "another closes it");
    let step = -1;
    const st = (u, rec) => { const k = u.stepper("##st", ["One", "Two", "Three"], 0); if (k >= 0) step = k; rec("st"); };
    const s = settle(c, st).st;
    click(c, st, s.x + s.width - 6, s.y + s.height / 2);
    ok(step === 2, `clicking the last step reports 2 (${step})`);
    c.close();
}

{
    const c = ctx();
    let col = 0x336699ff;
    const scene = (u, rec) => { col = u.color("##c", col); rec("c"); };
    const r = settle(c, scene).c;
    click(c, scene, r.x + 1, r.y + 21 + 4 + 10);
    ok((col >>> 24) === 0 && (col & 0xffffff) === 0x6699ff, `pressing at the start of the R slider zeroes red only (${(col >>> 0).toString(16)})`);
    c.close();
}

{
    const c = ctx();
    let shown = 0, pressed = 0;
    const scene = (u, rec) => { if (u.button("Open")) { pressed++; u.openPopup("pp"); } rec("b"); u.popup("pp", () => { shown++; u.label("in popup"); }); };
    const b = settle(c, scene).b;
    click(c, scene, b.x + 5, b.y + 5);
    c.frame((u) => scene(u, () => {}));
    ok(pressed === 1 && shown > 0, `a button's click opens its popup (shown ${shown})`);
    const n = shown;
    keys(c, scene, "escape");
    c.frame((u) => scene(u, () => {}));
    ok(shown <= n + 1, `Escape closes it (${shown - n} more frames)`);
    let menu = 0;
    const ctxScene = (u, rec) => { u.button("Target"); rec("t"); u.contextMenu("cm", () => { menu++; u.menuItem("Copy"); }); };
    const t = settle(c, ctxScene).t;
    click(c, ctxScene, t.x + 5, t.y + 5, { buttons: 2 });
    ok(menu > 0, "a right press over the widget opens its context menu");
    c.close();
}

{
    const c = ctx();
    let modal = 0, closed = false;
    const scene = (u, rec) => { u.button("x"); rec("b"); if (u.modal("##m", () => { modal++; if (u.button("Close")) { closed = true; u.closePopup(); } rec("close"); }) === false && modal) {} };
    c.frame((u) => { u.openModal("##m"); scene(u, () => {}); });
    let rects = settle(c, scene);
    ok(modal > 0 && rects.close, "openModal shows the modal");
    const n = modal;
    click(c, scene, rects.close.x + 5, rects.close.y + 5);
    c.frame((u) => scene(u, () => {}));
    c.frame((u) => scene(u, () => {}));
    ok(closed && modal <= n + 2, "closePopup() inside it closes the modal");
    c.close();
}

{
    const c = ctx();
    const scene = (u, rec) => { u.button("Hover me"); rec("b"); u.tooltip("tip", { delay: 0.5 }); };
    const b = settle(c, scene).b;
    const tipPixel = () => { const px = c.pixels(), x = Math.round(b.x + 5 + 12 + 3), y = Math.round(b.y + 5 + 16 + 3), i = (y * W + x) * 4; return ((px[i] << 24) | (px[i + 1] << 16) | (px[i + 2] << 8) | px[i + 3]) >>> 0; };
    c.setTime(1);
    c.pointer({ x: b.x + 5, y: b.y + 5 });
    c.frame((u) => scene(u, () => {}));
    const bg = tipPixel();
    ok(c.wakeIn > 0 && c.wakeIn <= 0.5, `hovering asks for the frame that shows the tooltip (wakeIn ${c.wakeIn})`);
    c.setTime(1.6);
    c.frame((u) => scene(u, () => {}));
    ok(tipPixel() !== bg, "after the delay the tooltip is drawn at the pointer");
    c.close();
}

{
    const c = ctx();
    const img = c.addImage(new Uint8Array(64).fill(255), 4, 4);
    let hits = 0, rail = 0;
    const scene = (u, rec) => { if (u.iconButton("##ib", img, 16)) hits++; rec("ib"); };
    const r = settle(c, scene).ib;
    click(c, scene, r.x + r.width / 2, r.y + r.height / 2);
    ok(hits === 1, `an icon button clicks once (${hits})`);
    const rs = (u, rec) => { rail = u.navRail("##rail", ["Inbox", "Sent", "Trash"], rail); rec("rail"); };
    const n = settle(c, rs).rail;
    click(c, rs, n.x + n.width / 2, n.y + 8 + 2 * (21 + 4) + 10);
    ok(rail === 2, `clicking the third item of a nav rail (8 + 2 x 25 points down) selects it (${rail})`);
    c.removeImage(img);
    c.close();
}

{
    const c = new ui.Context({ width: 40, height: 30, scale: 2, clock: "manual" });
    const buf = new Uint8Array(80 * 60 * 4);
    let e = null;
    try { c.pixelsInto(buf); } catch (x) { e = x; }
    ok(e instanceof TypeError && /before the first frame/.test(e.message), `pixelsInto before any frame is refused (${e})`);
    c.frame((u) => u.button("B"));
    const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
    ok(c.pixelsInto(buf) === buf, "pixelsInto returns the array it was given");
    ok(same(buf, c.pixels()), "it holds the same bytes pixels() returns (rasterised straight into the target)");
    buf.fill(7);
    c.pixelsInto(buf);
    ok(same(buf, c.pixels()), "and again from the cached raster of the same frame");
    c.pointer({ x: 5, y: 5 });
    c.frame((u) => u.button("B"));
    c.pixelsInto(buf);
    ok(same(buf, c.pixels()), "after a frame that changed (hover), it holds the new frame");
    for (const [bad, re, why] of [[new Uint8Array(10), /80 \* 60|19200/, "a wrong size"], [new Uint8ClampedArray(19200), /Uint8Array/, "another typed array"], [[1, 2], /Uint8Array/, "a plain array"]]) {
        let err = null;
        try { c.pixelsInto(bad); } catch (x) { err = x; }
        ok(err !== null && re.test(err.message), `${why} is refused naming what it needs (${err && err.message})`);
    }
    const view = new Uint8Array(new ArrayBuffer(19200 + 16), 8, 19200);
    c.pixelsInto(view);
    ok(same(view, c.pixels()) && new Uint8Array(view.buffer, 0, 8).every((v) => v === 0) && new Uint8Array(view.buffer, 19208, 8).every((v) => v === 0),
        "an offset view is written within its own bytes only");
    c.close();
}

// lastRect() after a scroll or panel block is the container's box, not its last child's: a wheel
// aimed at the scroll's centre must land inside the scroll, which holds more rows than it shows.
{
    const c = ctx();
    const scene = (u, rec) => {
        u.scroll("list", { height: 100 }, () => { for (let i = 0; i < 40; i++) { u.button("row " + i); rec("row" + i); } });
        rec("scroll");
        u.panel("side", { width: 120, height: 50 }, () => { u.button("in panel"); rec("inner"); });
        rec("panel");
    };
    const r = settle(c, scene);
    ok(r.scroll && r.scroll.height === 100 && r.row39.y > r.scroll.y + r.scroll.height, `lastRect after scroll() is the scroll area (${JSON.stringify(r.scroll)})`);
    ok(r.panel && r.panel.width === 120 && r.panel.height === 50, `lastRect after panel() is the panel (${JSON.stringify(r.panel)})`);
    const [x, y] = mid(r.scroll), y0 = r.row0.y;
    c.pointer({ x, y, buttons: 0, wheelY: -1 });
    const after = settle(c, scene);
    ok(after.row0.y < y0, `a wheel at the centre of lastRect() scrolls the list (${y0} -> ${after.row0.y})`);
    c.close();
}

print(`test_ui_interact: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failures`);
