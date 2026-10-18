// Scale invariance: every length in dyna:ui is in logical points, so a frame laid out at an integer
// scale k must be k times the frame at scale 1. Text is excluded (glyph bitmaps round per size) and
// so is tessellation density (a circle gets more segments when larger); what is compared is, per
// colour, the bounding box of the opaque solid geometry (vertices sampling the atlas's white texel).
// A length written in device pixels (a literal 2, a 10 px thumb, a 100 px default column) moves a
// box by (k - 1) times itself and fails here, while every scale-1 test still passes.
// The font is pinned to ProggyClean, whose advance (7) and line height (13) scale exactly.
// Every Context method either appears in a scene or is listed as not drawing, so a new widget
// fails this file until it is added.
import * as ui from "dyna:ui";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }

const W = 300, H = 260;
const NOT_DRAWING = ["activated", "addImage", "atlas", "close", "closePopup", "closed", "composition", "dispose", "dragging", "editing", "frame", "geometry",
    "height", "id", "idle", "insets", "key", "lastRect", "loadFont", "openDrawer", "openModal", "openPopup", "openSheet", "pixelHeight", "pixelWidth", "pixels", "pixelsInto",
    "pointer", "removeImage", "resize", "scale", "scroll", "setTime", "sizeClass", "stats", "submitted", "textInput", "theme", "type", "updateImage", "wakeIn",
    "width"];

const img = { handle: 0 }, toasted = new WeakSet();
const scenes = {
    text: (u) => { u.label("Label"); u.heading("Head"); u.bullet("Bullet"); u.text("Wrapped text that runs on."); u.link("Link"); u.badge("7"); u.separator("T"); u.spacer(10, 4); },
    buttons: (u) => { u.button("Button"); u.toggle("Tog", true); u.iconButton("ib", img.handle, 16); u.image(img.handle, 16, 8); },
    choices: (u) => { u.checkbox("Check", true); u.switch("Sw", true); u.radio("R", ["a", "b"], 1); u.segmented("Seg", ["x", "y"], 0); u.tabs("Tab", ["p", "q"], 1); u.selectable("Sel", true); u.chip("chip", true); },
    values: (u) => { u.slider("S", 0.3, 0, 1); u.slider("V", 0.6, 0, 1, { vertical: true, length: 40 }); u.range("R", [0.2, 0.7], 0, 1); u.drag("D", 5); u.spin("P", 3); u.progress(0.5); u.spinner(); u.rating("Rt", 3); },
    fields: (u) => { u.textField("F", "abc"); u.textArea("A", "two\nlines", { rows: 2 }); u.search("Q", "x"); u.textField("E", "", { error: "bad", help: "hint" }); },
    inputs: (u) => { u.autocomplete("Ac", "b", ["bar", "baz"]); u.timePicker("Tp", { hour: 13, minute: 5 }, { h12: true }); u.tagInput("Tg", ["one", "two"]); u.accordion("Acc", ["A", "B"], 1, (u) => u.label("body")); },
    scaffold: (u) => { u.scaffold("Sc", ["A", "B", "C"], 1, (u) => u.label("page")); },
    drag: (u) => { u.row(() => { u.button("src"); u.dragSource("p", { label: "moving" }); u.column({ id: "tgt", width: 80, height: 60 }, () => u.label("t")); u.dropTarget(); }); },
    lists: (u) => { u.list("L", ["a", "b", "c"], 1, { rows: 3 }); u.combo("C", ["one", "two"], 0); u.log("lg", ["one", "two"], { rows: 2 }); },
    table: (u) => { u.table("T", { columns: [{ name: "A" }, { name: "B", width: 50 }], rows: 3, cell: (r, c) => `${r}${c}`, visibleRows: 3 }); },
    bars: (u) => { u.menubar(() => { u.menu("File", () => { u.menuItem("Open"); }); }); u.toolbar(() => { u.button("T1"); u.button("T2"); }); u.statusbar(() => u.label("ok")); u.label("body"); },
    containers: (u) => { u.row(() => { u.label("r1"); u.label("r2"); }); u.column(() => u.label("c")); u.box({ fill: 0x445566ff, width: 30, height: 10 }, () => {}); u.wrap(() => { for (let k = 0; k < 6; k++) u.chip("w" + k, false); }); u.panel("pn", () => u.label("p")); },
    groups: (u) => { u.group("G", () => u.label("in")); u.collapsing("C", true); u.card("Card", { width: 120 }, () => u.label("c")); u.property("prop", () => u.label("v")); u.tree("N", { open: true }, () => { u.tree("leaf", { leaf: true }); }); },
    window: (u) => { u.window("W", { x: 20, y: 20, width: 120, height: 80, resizable: true }, () => u.label("in")); },
    split: (u) => { u.split("S", 0.4, (u) => u.label("a"), (u) => u.label("b")); },
    charts: (u) => { u.plot("P", [1, 3, 2, 4], {}); u.bars("B", [1, 2, 3], {}); },
    nav: (u) => { u.pagination("pg", 3, 10); u.breadcrumbs("bc", ["a", "b"]); u.stepper("st", ["a", "b", "c"], 1); u.navRail("rail", ["A", "B"], 0); },
    people: (u) => { u.avatar("Ada"); u.skeleton(40, 10); u.color("col", 0x336699ff); },
    date: (u) => { u.datePicker("dp", { year: 2026, month: 10, day: 30 }); },
    mobile: (u) => { u.appBar("Title", () => u.button("M")); u.label("content"); u.navBar("tabs", ["A", "B"], 1); u.fab("##add"); u.fab("Compose"); },
    overlays: (u) => { if (!toasted.has(u)) { toasted.add(u); u.toast("toast"); } u.openSheet("##sh"); u.sheet("##sh", () => u.label("sheet")); },
    drawer: (u) => { u.openDrawer("dr"); u.drawer("dr", () => u.label("drawer")); },
    modal: (u) => { u.openModal("md"); u.modal("md", () => u.label("modal")); },
    popup: (u) => { u.button("anchor"); u.openPopup("pp"); u.popup("pp", () => u.label("popup")); },
    context: (u) => { u.button("ctx"); u.contextMenu("cm", () => u.menuItem("Item")); u.tooltip("tip", { delay: 0 }); },
};

function boxes(scale, name, scene, insets) {
    const c = new ui.Context({ width: W, height: H, scale, insets, clock: "manual" });
    c.theme({ font: 0, radius: 0 });
    img.handle = c.addImage(new Uint8Array(64).fill(200), 4, 4);
    c.frame(scene);
    if (name === "context") c.pointer({ x: 20, y: 15, buttons: 2 });
    c.frame(scene);
    if (name === "context") { c.pointer({ x: 20, y: 15, buttons: 0 }); c.frame(scene); }
    if (name === "drag") { c.pointer({ x: 20, y: 38, buttons: 1 }); c.frame(scene); c.pointer({ x: 100, y: 30, buttons: 1 }); c.frame(scene); c.frame(scene); }
    c.frame(scene);
    const g = c.geometry(), a = c.atlas(), m = new Map(), v = g.vertices;
    c.close();
    for (let i = 0; i < v.length; i += 8) {
        if (v[i + 6] * a.width > 0.75 || v[i + 7] * a.height > 0.75 || v[i + 5] < 1) continue;
        const key = [2, 3, 4].map((o) => Math.round(v[i + o] * 255)).join(",");
        const b = m.get(key) || [Infinity, Infinity, -Infinity, -Infinity];
        b[0] = Math.min(b[0], v[i]); b[1] = Math.min(b[1], v[i + 1]); b[2] = Math.max(b[2], v[i]); b[3] = Math.max(b[3], v[i + 1]);
        m.set(key, b);
    }
    return m;
}

for (const insets of [[0, 0, 0, 0], [6, 20, 10, 12]]) {
    for (const [name, scene] of Object.entries(scenes)) {
        const a = boxes(1, name, scene, insets);
        ok(a.size >= 1, `${name}: the scene draws solid geometry (${a.size} colours)`);
        for (const k of [2, 3]) {
            const b = boxes(k, name, scene, insets), bad = [];
            for (const [key, ba] of a) {
                const bb = b.get(key);
                if (!bb) bad.push(`colour ${key} missing`);
                else if (ba.some((x, j) => Math.abs(bb[j] - k * x) > 1)) bad.push(`colour ${key}: [${ba}] x${k} expected, got [${bb}]`);
            }
            for (const key of b.keys()) if (!a.has(key)) bad.push(`colour ${key} only at x${k}`);
            ok(bad.length === 0, `${name} with insets [${insets}] at scale ${k} is ${k} x scale 1: ${bad.slice(0, 2).join("; ")}`);
        }
    }
}

{
    const c = new ui.Context({ width: 10, height: 10 });
    const names = [];
    for (let p = Object.getPrototypeOf(c); p && p !== Object.prototype; p = Object.getPrototypeOf(p))
        for (const k of Object.getOwnPropertyNames(p)) if (k !== "constructor") names.push(k);
    c.close();
    const src = Object.values(scenes).map(String).join("\n");
    const unseen = names.filter((n) => !NOT_DRAWING.includes(n) && !src.includes(`.${n}(`));
    ok(unseen.length === 0, `every drawing method is in a scene (missing: ${unseen.join(", ")})`);
    const stale = NOT_DRAWING.filter((n) => !names.includes(n));
    ok(stale.length === 0, `every name listed as not drawing exists (stale: ${stale.join(", ")})`);
}

print(`test_ui_scale: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failures`);
