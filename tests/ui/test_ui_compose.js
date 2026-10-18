// dyna:ui composition: lastRect(item, column) for the items of composite widgets, lastRect() after
// a block, table cell selection and in-cell editing, drag-and-drop, and the composite inputs
// (searchable combo, autocomplete, timePicker, tagInput, accordion, scaffold, error/help lines).
// Every expectation is a VALUE from the dynajs.d.ts contract: which item a click on item k's box
// selects, that a block's box contains its children, what a drop returns. Boxes are found with
// lastRect(), never computed from layout constants.
import * as ui from "dyna:ui";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }

const W = 480, H = 400;
function ctx(opts = {}) { return new ui.Context({ width: W, height: H, clock: "manual", ...opts }); }
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
const inside = (a, b) => a && b && a.x >= b.x - 0.01 && a.y >= b.y - 0.01 && a.x + a.width <= b.x + b.width + 0.01 && a.y + a.height <= b.y + b.height + 0.01;
const same = (a, b) => a && b && a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;
const disjoint = (a, b) => a.x + a.width <= b.x + 0.01 || b.x + b.width <= a.x + 0.01 || a.y + a.height <= b.y + 0.01 || b.y + b.height <= a.y + 0.01;
let T = 1;
const run = (c, scene) => { c.setTime((T += 0.05)); return c.frame(scene); };
function settle(c, scene) { run(c, scene); run(c, scene); run(c, scene); }
function click(c, scene, [x, y], kind) {
    const k = kind ? { kind } : {};
    c.pointer({ x, y, buttons: 1, ...k }); run(c, scene);
    c.pointer({ x, y, buttons: 0, ...k }); run(c, scene);
}
function key(c, scene, name, mods) { c.key(name, true, mods); run(c, scene); c.key(name, false, mods); run(c, scene); }
function typeText(c, scene, s) { c.type(s); run(c, scene); }

// A. lastRect(item) for every composite: item k's box lies inside the widget's box, items are
// disjoint, and a click at the centre of item k selects (or reports) k.
{
    const c = ctx();
    const items = ["Alpha", "Beta", "Gamma", "Delta"];
    const st = { tabs: 0, seg: 0, radio: 0, nav: 0, step: -1, crumb: -1, page: 0, rate: 0 };
    const R = {};
    const rec = (u, name, n) => { R[name] = { box: u.lastRect(), items: Array.from({ length: n }, (_, k) => u.lastRect(k)) }; };
    const scene = (u) => {
        st.tabs = u.tabs("##tabs", items, st.tabs); rec(u, "tabs", 4);
        st.seg = u.segmented("##seg", items, st.seg); rec(u, "seg", 4);
        u.row(() => { st.radio = u.radio("##radio", items, st.radio, { horizontal: true }); rec(u, "radio", 4); });
        const s = u.stepper("##step", items, 1); if (s >= 0) st.step = s; rec(u, "step", 4);
        const b = u.breadcrumbs("##crumbs", items); if (b >= 0) st.crumb = b; rec(u, "crumb", 4);
        st.page = u.pagination("##pages", st.page, 5); R.page = { box: u.lastRect(), items: [0, 1, 2, 3, 4].map((p) => u.lastRect(p)) };
        st.rate = u.rating("##rate", st.rate, { max: 4 }); rec(u, "rate", 4);
        u.row({ height: 120 }, () => { st.nav = u.navRail("##nav", items, st.nav); rec(u, "nav", 4); });
    };
    settle(c, scene);
    for (const name of ["tabs", "seg", "radio", "step", "crumb", "page", "rate", "nav"]) {
        const { box, items: it } = R[name];
        ok(box !== null && it.every((r) => r !== null), `${name}: lastRect() and lastRect(k) are laid out (${JSON.stringify(it)})`);
        ok(it.every((r) => inside(r, box)), `${name}: every item box lies inside the widget's box`);
        let dj = true;
        for (let a = 0; a < it.length; a++) for (let b = a + 1; b < it.length; b++) if (!disjoint(it[a], it[b])) dj = false;
        ok(dj, `${name}: item boxes do not overlap`);
    }
    for (const k of [3, 1, 2]) {
        click(c, scene, mid(R.tabs.items[k])); ok(st.tabs === k, `tabs: a click on item ${k}'s box selects ${k} (${st.tabs})`);
        click(c, scene, mid(R.seg.items[k])); ok(st.seg === k, `segmented: item ${k} (${st.seg})`);
        click(c, scene, mid(R.radio.items[k])); ok(st.radio === k, `radio: item ${k} (${st.radio})`);
        click(c, scene, mid(R.step.items[k])); ok(st.step === k, `stepper: item ${k} (${st.step})`);
        click(c, scene, mid(R.rate.items[k])); ok(st.rate === k + 1, `rating: star ${k} gives ${k + 1} (${st.rate})`);
        click(c, scene, mid(R.nav.items[k])); ok(st.nav === k, `navRail: item ${k} (${st.nav})`);
    }
    click(c, scene, mid(R.crumb.items[1])); ok(st.crumb === 1, `breadcrumbs: crumb 1 (${st.crumb})`);
    ok(R.crumb.items[3] !== null, "breadcrumbs: the last crumb (the current page) has a box too");
    click(c, scene, mid(R.page.items[3])); ok(st.page === 3, `pagination: lastRect(page) is page 3's button (${st.page})`);
    settle(c, scene);
    click(c, scene, mid(R.page.items[0])); ok(st.page === 0, `pagination: page 0 (${st.page})`);
    let after = "x";
    run(c, (u) => { scene(u); u.button("plain"); after = u.lastRect(0); });
    ok(after === null, "lastRect(item) after a widget without items is null");
    c.close();
}

// A. combo items while open, list rows, table rows and cells, datePicker days.
{
    const c = ctx();
    const items = ["one", "two", "three", "four", "five"];
    let sel = 0, lsel = -1, tsel = -1, date = { year: 2026, month: 2, day: 1 };
    const R = {};
    const scene = (u) => {
        sel = u.combo("##combo", items, sel); R.combo = u.lastRect(); R.c3 = u.lastRect(3);
        lsel = u.list("##list", items, lsel, { rows: 3 }); R.list = u.lastRect(); R.l1 = u.lastRect(1); R.l4 = u.lastRect(4);
        const t = u.table("##table", { columns: [{ name: "a", width: 60 }, { name: "b", width: 60 }], rows: 4, visibleRows: 4, cell: (r, k) => `${r}:${k}`, selected: tsel });
        tsel = t.selected; R.table = u.lastRect(); R.t2 = u.lastRect(2); R.t21 = u.lastRect(2, 1); R.t20 = u.lastRect(2, 0);
    };
    settle(c, scene);
    ok(R.c3 === null, "combo: lastRect(k) is null while the popup is closed");
    click(c, scene, mid(R.combo)); settle(c, scene);
    ok(R.c3 !== null && R.c3.y > R.combo.y, `combo: with the popup open, lastRect(3) is the item below the box (${JSON.stringify(R.c3)})`);
    click(c, scene, mid(R.c3)); ok(sel === 3, `combo: a click on lastRect(3) picks item 3 (${sel})`);
    ok(R.l1 && inside(R.l1, R.list), "list: row 1 lies inside the list");
    ok(R.l4 === null, "list: a row scrolled out of the 3 visible rows has no box");
    click(c, scene, mid(R.l1)); ok(lsel === 1, `list: a click on row 1's box selects 1 (${lsel})`);
    ok(inside(R.t20, R.t2) && inside(R.t21, R.t2) && R.t21.x > R.t20.x, "table: lastRect(row, column) are cells inside lastRect(row), in column order");
    click(c, scene, mid(R.t21)); ok(tsel === 2, `table: a click on cell (2, 1) selects row 2 (${tsel})`);
    let dr = {};
    const ds = (u) => { date = u.datePicker("##date", date); dr.d15 = u.lastRect(15); dr.box = u.lastRect(); dr.d0 = u.lastRect(0); };
    settle(c, ds);
    ok(dr.d15 && inside(dr.d15, dr.box) && dr.d0 === null, "datePicker: lastRect(day) is that day's cell; day 0 does not exist");
    click(c, ds, mid(dr.d15)); ok(date.day === 15, `datePicker: a click on lastRect(15) picks day 15 (${date.day})`);
    c.close();
}

// B. lastRect() after a block is the block's own box (it contains every child), not the last child.
{
    const c = ctx({ width: 640, height: 900 });
    const R = {};
    let menuOpen = false;
    const scene = (u) => {
        u.group("Group", () => { u.button("g1"); u.button("g2"); R.gChild = u.lastRect(); }); R.group = u.lastRect();
        u.card("Card", () => { u.button("c1"); R.cChild = u.lastRect(); }); R.card = u.lastRect();
        u.property("Prop", () => { u.button("p1"); R.pChild = u.lastRect(); }); R.prop = u.lastRect();
        u.row({ id: "r" }, () => { u.button("r1"); u.button("r2"); R.rChild = u.lastRect(); }); R.row = u.lastRect();
        u.tree("Node", { open: true }, (u) => { u.button("t1"); R.tChild = u.lastRect(); }); R.tree = u.lastRect();
        u.split("##split", 0.5, (u) => u.button("s1"), (u) => { u.button("s2"); R.sChild = u.lastRect(); }); R.split = u.lastRect();
        u.menubar(() => { menuOpen = u.menu("File", (u) => { u.menuItem("Open"); R.mChild = u.lastRect(); }); R.menu = u.lastRect(); });
    };
    settle(c, scene);
    for (const [blk, child] of [["group", "gChild"], ["card", "cChild"], ["prop", "pChild"], ["row", "rChild"], ["split", "sChild"]]) {
        ok(R[blk] && R[child] && inside(R[child], R[blk]) && !same(R[child], R[blk]), `${blk}: lastRect() after the block contains its last child and is not it (${JSON.stringify(R[blk])} vs ${JSON.stringify(R[child])})`);
    }
    ok(R.tree && R.tChild && R.tree.y < R.tChild.y && !same(R.tree, R.tChild), "tree: lastRect() after an open node is its header row, not its last child");
    click(c, scene, mid(R.menu)); settle(c, scene);
    ok(menuOpen && R.mChild && R.menu && !same(R.menu, R.mChild) && R.menu.y < R.mChild.y, "menu: lastRect() after an open menu is its title, not its last item");
    c.close();
}

// B. overlays, docked bars, columns and wraps with an id, and log rows.
{
    const c = ctx({ width: 640, height: 700 });
    const R = {};
    const scene = (u) => {
        u.menubar(() => { u.menu("File", () => u.menuItem("Open")); R.mbChild = u.lastRect(); }); R.menubar = u.lastRect();
        u.toolbar(() => { u.button("tb1"); u.button("tb2"); R.tbChild = u.lastRect(); }); R.toolbar = u.lastRect();
        u.statusbar(() => { u.label("ready"); u.label("ln 1"); R.sbChild = u.lastRect(); }); R.statusbar = u.lastRect();
        u.column({ id: "col" }, () => { u.button("c1"); u.button("c2"); R.colChild = u.lastRect(); }); R.col = u.lastRect();
        u.wrap({ id: "wr" }, () => { u.chip("w1", false); u.chip("w2", false); R.wrChild = u.lastRect(); }); R.wrap = u.lastRect();
        u.log("##log", ["l0", "l1", "l2", "l3"], { rows: 4 }); R.log = u.lastRect(); R.log2 = u.lastRect(2);
    };
    settle(c, scene);
    for (const [blk, child] of [["menubar", "mbChild"], ["toolbar", "tbChild"], ["statusbar", "sbChild"], ["col", "colChild"], ["wrap", "wrChild"]]) {
        ok(R[blk] && R[child] && inside(R[child], R[blk]) && !same(R[child], R[blk]), `${blk}: lastRect() after it contains its last child and is not it (${JSON.stringify(R[blk])} vs ${JSON.stringify(R[child])})`);
    }
    ok(R.statusbar.width === 640 && R.statusbar.y + R.statusbar.height === 700, `the statusbar docks to the bottom edge (${JSON.stringify(R.statusbar)})`);
    ok(R.log2 && inside(R.log2, R.log), "log: lastRect(row) is that line, inside the log");
    c.close();
    for (const kind of ["popup", "modal", "drawer", "sheet"]) {
        const c = ctx({ width: 640, height: 700 });
        let opened = false, box = null, child = null;
        const scene = (u) => {
            u.button("anchor");
            if (!opened) { opened = true; u[{ popup: "openPopup", modal: "openModal", drawer: "openDrawer", sheet: "openSheet" }[kind]]("ov"); }
            u[kind]("ov", () => { u.button("o1"); u.button("o2"); child = u.lastRect(); });
            box = u.lastRect();
        };
        settle(c, scene);
        ok(box && child && inside(child, box) && !same(child, box), `${kind}: lastRect() after it is its panel, containing its last child (${JSON.stringify(box)} vs ${JSON.stringify(child)})`);
        c.close();
    }
}

// F. label-like widgets have a box; duplicates in one scope answer the first.
{
    const c = ctx();
    const R = {};
    const scene = (u) => {
        u.label("Name"); R.label = u.lastRect();
        u.heading("Head"); R.head = u.lastRect();
        u.text("some text"); R.text = u.lastRect();
        u.bullet("point"); R.bullet = u.lastRect();
        u.badge("3"); R.badge = u.lastRect();
        u.progress(0.5, 100); R.prog = u.lastRect();
        u.label("Name"); R.dup = u.lastRect();
    };
    settle(c, scene);
    for (const k of ["label", "head", "text", "bullet", "badge", "prog"]) ok(R[k] !== null && R[k].width > 0, `${k}: lastRect() after it is its box`);
    ok(R.label.y < R.head.y && R.head.y < R.text.y && R.text.y < R.bullet.y, "label boxes follow declaration order");
    ok(R.prog.width === 100, `progress keeps the requested width (${R.prog.width})`);
    ok(same(R.dup, R.label), "a second label with the same text in one scope answers the first one's box");
    c.close();
}

// C. table cell selection, keyboard movement, activation and in-cell editing.
{
    const c = ctx();
    const data = [["a0", "b0", "c0"], ["a1", "b1", "c1"], ["a2", "b2", "c2"]];
    let sel = -1, col = -1, editing = null, res = null, activations = 0;
    const R = {};
    const scene = (u) => {
        res = u.table("##grid", {
            columns: [{ name: "A", width: 60 }, { name: "B", width: 60 }, { name: "C", width: 60 }],
            rows: 3, visibleRows: 3, cell: (r, k) => data[r][k], selected: sel, selectedColumn: col,
            ...(editing !== null ? { edit: editing } : {}),
        });
        sel = res.selected; col = res.column;
        if (res.activated) { activations++; if (editing === null) editing = data[sel][col]; }
        if (editing !== null && res.edit !== undefined) editing = res.edit;
        if (res.committed) { data[sel][col] = editing; editing = null; }
        if (res.cancelled) editing = null;
        for (let r = 0; r < 3; r++) for (let k = 0; k < 3; k++) R[`${r}${k}`] = u.lastRect(r, k);
    };
    settle(c, scene);
    ok(res.column === -1 && res.selected === -1, `cell mode starts with nothing selected (${res.selected}, ${res.column})`);
    click(c, scene, mid(R["12"]));
    ok(sel === 1 && col === 2, `a click on cell (1, 2) selects row 1, column 2 (${sel}, ${col})`);
    key(c, scene, "left"); ok(col === 1 && sel === 1, `Left moves one column (${sel}, ${col})`);
    key(c, scene, "down"); ok(sel === 2 && col === 1, `Down moves one row and keeps the column (${sel}, ${col})`);
    key(c, scene, "left"); key(c, scene, "left"); ok(col === 0, `Left stops at the first column (${col})`);
    key(c, scene, "end"); ok(col === 2, `End goes to the last column (${col})`);
    key(c, scene, "home"); ok(col === 0, `Home goes to the first column (${col})`);
    key(c, scene, "return");
    ok(activations === 1 && editing === "a2", `Return activates the selected cell (${activations}, ${editing})`);
    run(c, scene);
    typeText(c, scene, "Z");
    ok(editing === "a2Z", `the activated cell is a focused text field holding edit; typing appends (${editing})`);
    key(c, scene, "return");
    ok(editing === null && data[2][0] === "a2Z", `Return commits the edit (${data[2][0]})`);
    key(c, scene, "right"); ok(col === 1, `after a commit the arrows move between cells again (${col})`);
    key(c, scene, "return"); run(c, scene);
    typeText(c, scene, "Q");
    key(c, scene, "escape");
    ok(editing === null && data[2][1] === "b2", `Escape cancels: the cell keeps its text (${data[2][1]})`);
    const before = activations;
    c.pointer({ x: mid(R["00"])[0], y: mid(R["00"])[1], buttons: 1, time: 10 }); run(c, scene);
    c.pointer({ x: mid(R["00"])[0], y: mid(R["00"])[1], buttons: 0, time: 10.05 }); run(c, scene);
    c.pointer({ x: mid(R["00"])[0], y: mid(R["00"])[1], buttons: 1, time: 10.1 }); run(c, scene);
    ok(activations === before + 1 && sel === 0 && col === 0, `a double click on a cell selects and activates it (${activations - before}, ${sel}, ${col})`);
    c.pointer({ x: mid(R["00"])[0], y: mid(R["00"])[1], buttons: 0, time: 10.15 }); run(c, scene);
    c.close();
}

// A. a row-mode table answers lastRect(row, column) with the same boxes a cell-mode table lays out.
{
    const boxes = {};
    for (const mode of ["rows", "cells"]) {
        const c = ctx();
        const scene = (u) => {
            const cols = [{ name: "A", width: 70 }, { name: "B", width: 45 }, { name: "C", width: 90 }];
            u.table("##same", { columns: cols, rows: 6, visibleRows: 4, cell: (r, k) => `${r}/${k}`, ...(mode === "cells" ? { selectedColumn: 0 } : {}) });
            boxes[mode] = [];
            for (let r = 0; r < 4; r++) for (let k = 0; k < 4; k++) boxes[mode].push(u.lastRect(r, k));
        };
        settle(c, scene);
        c.close();
    }
    ok(boxes.rows.every((r, i) => (i % 4 === 3 ? r === null : r !== null)), "row mode: every visible cell has a box and column 3 of 3 does not exist");
    ok(boxes.rows.every((r, i) => (r === null ? boxes.cells[i] === null : same(r, boxes.cells[i]))), `row-mode cell boxes equal the laid-out cells of cell mode (${JSON.stringify(boxes.rows[5])} vs ${JSON.stringify(boxes.cells[5])})`);
}

// A. lastRect(-1, column) is a table header: above row 0, in column order, and a click on it sorts.
{
    const c = ctx();
    let sort = -1, desc = false, H = [], r0 = null, threw = [];
    const scene = (u) => {
        const r = u.table("##hdr", { columns: [{ name: "A", width: 60 }, { name: "B", width: 60 }], rows: 3, cell: (r, k) => `${r}${k}`, sort: { column: sort, descending: desc } });
        sort = r.sortColumn; desc = r.descending;
        H = [0, 1, 2].map((k) => u.lastRect(-1, k)); r0 = u.lastRect(0, 1);
        for (const bad of [[-1], [-2, 0], [0.5]]) { try { u.lastRect(...bad); threw.push("none"); } catch (e) { threw.push(e.name); } }
    };
    settle(c, scene);
    ok(H[0] && H[1] && H[2] === null && H[1].x > H[0].x && H[1].y + H[1].height <= r0.y + 0.01, `headers lie above row 0 in column order; a column past the last has none (${JSON.stringify(H)})`);
    ok(threw.slice(0, 3).join() === "RangeError,RangeError,RangeError", `lastRect(-1) without a column, -2 and fractions are refused (${threw.slice(0, 3)})`);
    click(c, scene, mid(H[1]));
    ok(sort === 1 && !desc, `a click on lastRect(-1, 1) sorts by column 1 (${sort}, ${desc})`);
    let after = "x";
    run(c, (u) => { u.list("##l", ["a"], 0); after = u.lastRect(-1, 0); });
    ok(after === null, "lastRect(-1, column) after a widget that is not a table is null");
    c.close();
}

// C. a row-mode table now reports Return and double click as activation (result and Context.activated).
{
    const c = ctx();
    let sel = -1, act = false, actRow = -1;
    const R = {};
    const scene = (u) => {
        const r = u.table("##rows", { columns: [{ name: "A" }], rows: 3, cell: (r) => `row ${r}`, selected: sel });
        sel = r.selected; act = r.activated; if (r.activated) actRow = u.activated; R.r1 = u.lastRect(1);
        ok(r.column === -1, "a table without selectedColumn reports column -1");
    };
    settle(c, scene);
    click(c, scene, mid(R.r1));
    ok(sel === 1 && actRow === -1, `a click selects row 1 without activating (${sel}, ${actRow})`);
    c.key("return", true); run(c, scene);
    ok(act && actRow === 1, `Return on the focused table activates the selected row; activated is 1 (${act}, ${actRow})`);
    c.key("return", false); run(c, scene);
    c.close();
}

// D. drag-and-drop: payload delivered once to the target under the pointer, click suppressed.
{
    const c = ctx();
    const R = {};
    let dropped = [], clicks = 0, dragging = null, srcFlag = false;
    const scene = (u) => {
        u.row(() => {
            if (u.button("Card A")) clicks++;
            srcFlag = u.dragSource("A", { label: "Card A" });
            R.src = u.lastRect();
            u.column({ id: "colB", width: 120, height: 100, fill: 0x30303080 }, () => { u.label("inner"); });
            R.tgt = u.lastRect();
            const p = u.dropTarget(); if (p !== null) dropped.push(p);
            u.column({ id: "colC", width: 120, height: 100, fill: 0x30303080 }, () => {
                u.button("nested"); R.nested = u.lastRect();
                const q = u.dropTarget(); if (q !== null) dropped.push("nested:" + q);
            });
        });
        dragging = u.dragging;
    };
    settle(c, scene);
    click(c, scene, mid(R.src));
    ok(clicks === 1 && dropped.length === 0, `a click without movement on a drag source is still a click (${clicks})`);
    const [sx, sy] = mid(R.src), [tx, ty] = mid(R.tgt);
    c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene);
    c.pointer({ x: sx + 1, y: sy, buttons: 1 }); run(c, scene);
    ok(dragging === null && !srcFlag, "a move inside dragSlop does not start a drag");
    c.pointer({ x: (sx + tx) / 2, y: sy + 20, buttons: 1 }); run(c, scene);
    ok(dragging === "A" && srcFlag, `past dragSlop the source drags and dragging is its payload (${dragging})`);
    c.pointer({ x: tx, y: ty, buttons: 1 }); run(c, scene); run(c, scene);
    ok(dropped.length === 0, "nothing drops while the button is held");
    c.pointer({ x: tx, y: ty, buttons: 0 }); run(c, scene);
    ok(dropped.length === 1 && dropped[0] === "A", `releasing over the target delivers the payload once (${JSON.stringify(dropped)})`);
    ok(clicks === 1, `the drag's release is not a click on the source (${clicks})`);
    run(c, scene);
    ok(dragging === null && dropped.length === 1, "after the drop nothing is dragging and nothing drops again");
    const [nx, ny] = mid(R.nested);
    c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene);
    c.pointer({ x: nx, y: ny, buttons: 1 }); run(c, scene); run(c, scene);
    c.pointer({ x: nx, y: ny, buttons: 0 }); run(c, scene);
    ok(dropped[1] === "nested:A", `over a target inside nothing else, the innermost containing target receives it (${dropped[1]})`);
    c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene);
    c.pointer({ x: tx, y: ty, buttons: 1 }); run(c, scene);
    key(c, scene, "escape");
    ok(dragging === null, "Escape cancels the drag");
    c.pointer({ x: tx, y: ty, buttons: 0 }); run(c, scene);
    ok(dropped.length === 2 && clicks === 1, `a cancelled drag drops nothing and is not a click (${dropped.length}, ${clicks})`);
    c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene);
    c.pointer({ x: tx, y: ty, buttons: 1 }); run(c, scene);
    c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene); run(c, scene);
    c.pointer({ x: sx, y: sy, buttons: 0 }); run(c, scene);
    ok(clicks === 1 && dropped.length === 2, `a drag released back over its own source is not a click (${clicks})`);
    c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene);
    c.pointer({ x: W - 5, y: H - 5, buttons: 1 }); run(c, scene);
    c.pointer({ x: W - 5, y: H - 5, buttons: 0 }); run(c, scene);
    ok(dropped.length === 2, "a release over no target drops nothing");
    let threw = 0;
    c.frame((u) => { u.button("x"); for (const bad of [{}, [], null, undefined, NaN, Infinity, () => 1]) { try { u.dragSource(bad); } catch (e) { if (e instanceof TypeError) threw++; } } });
    ok(threw === 7, `dragSource refuses payloads that are not a string or finite number (${threw}/7)`);
    c.close();
}

// D. nested drop targets: the smallest one under the pointer receives the drop, the outer one does not.
{
    const c = ctx();
    const R = {}, got = [];
    const scene = (u) => {
        u.row(() => {
            u.button("src"); u.dragSource(7); R.src = u.lastRect();
            u.column({ id: "outer", width: 260, height: 200, padding: 20 }, () => {
                u.column({ id: "inner", width: 100, height: 60 }, () => u.label("inner")); R.inner = u.lastRect();
                const q = u.dropTarget(); if (q !== null) got.push("inner:" + q);
            });
            R.outer = u.lastRect();
            const p = u.dropTarget(); if (p !== null) got.push("outer:" + p);
        });
    };
    settle(c, scene);
    ok(inside(R.inner, R.outer) && !same(R.inner, R.outer), "the inner target lies inside the outer one");
    const drop = ([x, y]) => {
        const [sx, sy] = mid(R.src);
        c.pointer({ x: sx, y: sy, buttons: 1 }); run(c, scene);
        c.pointer({ x, y, buttons: 1 }); run(c, scene); run(c, scene);
        c.pointer({ x, y, buttons: 0 }); run(c, scene); run(c, scene);
    };
    drop(mid(R.inner));
    ok(got.join() === "inner:7", `over the inner target only the inner one receives it (${got})`);
    drop([R.outer.x + R.outer.width - 10, R.outer.y + R.outer.height - 10]);
    ok(got.join() === "inner:7,outer:7", `outside the inner one the outer one receives it (${got})`);
    c.close();
}

// D. on touch a drag starts with a long press; a quick swipe still scrolls.
{
    let c = ctx({ touch: true });
    const R = {};
    let dropped = null, scrolled = 0;
    const scene = (u) => {
        u.row(() => {
            u.scroll("##list", { width: 160, height: 200 }, () => {
                for (let i = 0; i < 20; i++) { u.button("item " + i); if (i === 1) { u.dragSource(i); R.item = u.lastRect(); } }
            });
            R.list = u.lastRect();
            u.column({ id: "bin", width: 120, height: 120 }, () => u.label("bin"));
            R.bin = u.lastRect();
            const p = u.dropTarget(); if (p !== null) dropped = p;
        });
    };
    settle(c, scene);
    const [ix, iy] = mid(R.item);
    c.pointer({ x: ix, y: iy, buttons: 1, kind: "touch", time: 1 }); run(c, scene);
    for (let k = 1; k <= 4; k++) { c.pointer({ x: ix, y: iy - 30 * k, buttons: 1, kind: "touch", time: 1 + k * 0.02 }); run(c, scene); }
    c.pointer({ x: ix, y: iy - 120, buttons: 0, kind: "touch", time: 1.1 }); run(c, scene);
    for (let k = 0; k < 4; k++) run(c, scene);
    const moved = u0(R);
    ok(dropped === null && moved < iy, `a swipe on a drag source inside a scroll container scrolls it (item y ${iy} -> ${moved})`);
    c.close();
    c = ctx({ touch: true });
    settle(c, scene);
    const [jx, jy] = mid(R.item);
    c.pointer({ x: jx, y: jy, buttons: 1, kind: "touch", time: T }); run(c, scene);
    c.setTime((T += 1)); c.pointer({ x: jx, y: jy, buttons: 1, kind: "touch", time: T }); run(c, scene);
    const [bx, by] = mid(R.bin);
    c.pointer({ x: bx, y: by, buttons: 1, kind: "touch", time: T }); run(c, scene); run(c, scene);
    c.pointer({ x: bx, y: by, buttons: 0, kind: "touch", time: T }); run(c, scene);
    ok(dropped === 1, `after a long press the touch drags item 1 into the bin (${dropped})`);
    c.close();
    c = ctx({ touch: true });
    let got = null, drg = null;
    const flat = (u) => {
        u.row(() => {
            u.button("loose"); u.dragSource("L"); R.loose = u.lastRect();
            u.column({ id: "bin2", width: 120, height: 120 }, () => u.label("bin")); R.bin2 = u.lastRect();
            const p = u.dropTarget(); if (p !== null) got = p;
        });
        drg = u.dragging;
    };
    settle(c, flat);
    const [lx, ly] = mid(R.loose), [b2x, b2y] = mid(R.bin2);
    c.pointer({ x: lx, y: ly, buttons: 1, kind: "touch", time: T }); run(c, flat);
    c.pointer({ x: (lx + b2x) / 2, y: ly, buttons: 1, kind: "touch", time: T + 0.02 }); run(c, flat);
    ok(drg === "L", `a touch move past dragSlop that no scroll container takes starts the drag at once (${drg})`);
    c.pointer({ x: b2x, y: b2y, buttons: 1, kind: "touch", time: T + 0.04 }); run(c, flat); run(c, flat);
    c.pointer({ x: b2x, y: b2y, buttons: 0, kind: "touch", time: T + 0.06 }); run(c, flat);
    ok(got === "L", `and drops it (${got})`);
    c.close();
    function u0(r) { return r.item ? r.item.y + r.item.height / 2 : iy; }
}

// E. combo search and keyboard; autocomplete; timePicker; tagInput; accordion; scaffold.
{
    const c = ctx();
    const fruits = ["Apple", "Banana", "Cherry", "Date", "Elderberry", "Fig"];
    let sel = 0;
    const R = {};
    const scene = (u) => { sel = u.combo("##fruit", fruits, sel, { search: true }); R.box = u.lastRect(); R.items = fruits.map((_, k) => u.lastRect(k)); };
    settle(c, scene);
    click(c, scene, mid(R.box)); settle(c, scene);
    ok(R.items.every((r) => r !== null), "search combo: every item is shown before typing");
    typeText(c, scene, "rr"); settle(c, scene);
    ok(R.items[2] !== null && R.items[4] !== null && R.items[0] === null && R.items[1] === null, `typing filters the items by substring, case-insensitively (${R.items.map((r) => (r ? 1 : 0)).join("")})`);
    key(c, scene, "down"); key(c, scene, "return");
    ok(sel === 2, `Down then Return picks the first match below the current choice (${sel})`);
    click(c, scene, mid(R.box)); settle(c, scene);
    ok(R.items.every((r) => r !== null), "the filter is cleared when the popup opens again");
    key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "up"); key(c, scene, "return");
    ok(sel === 3, `arrows move the highlight from the current choice; Return picks it (${sel})`);
    c.close();
}
{
    const c = ctx();
    let sel = 1;
    const R = {};
    const scene = (u) => { sel = u.combo("##plain", ["a", "b", "c"], sel); R.box = u.lastRect(); };
    settle(c, scene);
    click(c, scene, mid(R.box));
    key(c, scene, "down"); key(c, scene, "return");
    ok(sel === 2, `a plain combo also takes Down and Return while open (${sel})`);
    c.close();
    const c2 = ctx();
    let shown = null;
    sel = 1;
    const s2 = (u) => { scene(u); shown = u.lastRect(0); };
    settle(c2, s2);
    key(c2, s2, "tab");
    ok(shown === null, "Tab focuses the closed combo");
    key(c2, s2, "return"); settle(c2, s2);
    ok(shown !== null && sel === 1, `Return on the keyboard-focused, closed combo opens it without picking (${shown !== null}, ${sel})`);
    key(c2, s2, "down"); key(c2, s2, "return");
    ok(sel === 2, `then Down and Return pick (${sel})`);
    c2.close();
}
{
    const c = ctx();
    const all = ["Amsterdam", "Athens", "Berlin", "Bern", "Bratislava"];
    let text = "";
    const R = {};
    const scene = (u) => {
        text = u.autocomplete("City", text, text ? all.filter((s) => s.toLowerCase().startsWith(text.toLowerCase())) : []);
        R.box = u.lastRect(); R.i1 = u.lastRect(1);
    };
    settle(c, scene);
    click(c, scene, mid(R.box));
    typeText(c, scene, "Be"); settle(c, scene);
    ok(text === "Be" && R.i1 !== null, `typing shows the caller's suggestions below the field (${text})`);
    click(c, scene, mid(R.i1));
    ok(text === "Bern", `a click on suggestion 1 replaces the text with it (${text})`);
    typeText(c, scene, "!");
    ok(text === "Bern!", `after a pick the caret is at the end of the picked text (${text})`);
    key(c, scene, "backspace");
    settle(c, scene);
    ok(R.i1 === null, "after a pick the suggestions close until the text changes");
    typeText(c, scene, "x"); settle(c, scene);
    key(c, scene, "backspace"); key(c, scene, "backspace"); settle(c, scene);
    key(c, scene, "down"); key(c, scene, "return");
    ok(text === "Berlin", `Down then Return picks the first suggestion (${text})`);
    c.close();
}
{
    const c = ctx();
    let t = { hour: 9, minute: 30 }, t12 = { hour: 23, minute: 5 };
    const R = {};
    const scene = (u) => {
        t = u.timePicker("Start", t, { step: 15 }); R.h = u.lastRect(0); R.m = u.lastRect(1);
        t12 = u.timePicker("End", t12, { h12: true }); R.ampm = u.lastRect(2); R.h12 = u.lastRect(0);
    };
    settle(c, scene);
    const keep = t;
    run(c, scene);
    ok(t === keep, "timePicker returns the same object while unchanged");
    click(c, scene, mid(R.h)); key(c, scene, "up");
    ok(t.hour === 10 && t.minute === 30, `Up on the focused hour adds an hour (${t.hour}:${t.minute})`);
    click(c, scene, mid(R.m)); key(c, scene, "up");
    ok(t.minute === 45, `Up on the minute adds the step (${t.minute})`);
    key(c, scene, "up");
    ok(t.minute === 0 && t.hour === 10, `minutes wrap within the hour (${t.hour}:${t.minute})`);
    typeText(c, scene, "4"); typeText(c, scene, "2");
    ok(t.minute === 42, `typing two digits sets the minute (${t.minute})`);
    click(c, scene, mid(R.ampm));
    ok(t12.hour === 11 && t12.minute === 5, `the AM/PM button moves by twelve hours (${t12.hour})`);
    click(c, scene, mid(R.h12)); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down"); key(c, scene, "down");
    ok(t12.hour === 23, `hours wrap through midnight (${t12.hour})`);
    let threw = 0;
    c.frame((u) => { for (const bad of [{ hour: 24, minute: 0 }, { hour: 1, minute: 60 }, { hour: 1.5, minute: 0 }, null]) { try { u.timePicker("x", bad); } catch (e) { threw++; } } });
    ok(threw === 4, `timePicker refuses an hour or minute out of range (${threw}/4)`);
    c.close();
}
{
    const c = ctx();
    let tags = ["red", "green"];
    const R = {};
    const scene = (u) => { tags = u.tagInput("Tags", tags, { hint: "add" }); R.box = u.lastRect(); R.t0 = u.lastRect(0); R.t1 = u.lastRect(1); };
    settle(c, scene);
    const keep = tags;
    run(c, scene);
    ok(tags === keep, "tagInput returns the same array while unchanged");
    ok(inside(R.t0, R.box) && inside(R.t1, R.box), "lastRect(k) is tag k's chip inside the field");
    click(c, scene, [R.box.x + R.box.width - 10, R.box.y + R.box.height / 2]);
    typeText(c, scene, "blue"); key(c, scene, "return");
    ok(tags.join() === "red,green,blue", `Return adds the typed tag (${tags})`);
    typeText(c, scene, " teal , pink");
    ok(tags.join() === "red,green,blue,teal", `a comma adds what precedes it, trimmed (${tags})`);
    key(c, scene, "return");
    ok(tags.join() === "red,green,blue,teal,pink", `and the rest stays typed (${tags})`);
    typeText(c, scene, "red"); key(c, scene, "return");
    ok(tags.length === 5, `a duplicate is not added (${tags})`);
    key(c, scene, "backspace");
    ok(tags.join() === "red,green,blue,teal", `Backspace in the empty field removes the last tag (${tags})`);
    settle(c, scene);
    click(c, scene, [R.t0.x + R.t0.width - 8, R.t0.y + R.t0.height / 2]);
    ok(tags.join() === "green,blue,teal", `the chip's close button removes that tag (${tags})`);
    c.close();
}
{
    const c = ctx();
    let open = 0, shown = [];
    const R = {};
    const scene = (u) => {
        shown = [];
        open = u.accordion("##acc", ["One", "Two", "Three"], open, (u, i) => { shown.push(i); u.label("body " + i); });
        R.box = u.lastRect(); R.h = [0, 1, 2].map((k) => u.lastRect(k));
    };
    settle(c, scene);
    ok(shown.join() === "0", `only the open section's fn runs, with its index (${shown})`);
    ok(R.h.every((r) => inside(r, R.box)), "lastRect(k) is header k, inside the accordion");
    click(c, scene, mid(R.h[2]));
    ok(open === 2 && shown.join() === "0", `the click frame returns 2 and still draws the section that was open (${open}, ${shown})`);
    run(c, scene);
    ok(open === 2 && shown.join() === "2", `the next frame shows section 2 only (${open}, ${shown})`);
    settle(c, scene);
    click(c, scene, mid(R.h[2])); run(c, scene);
    ok(open === -1 && shown.length === 0, `a click on the open header closes it (${open})`);
    c.close();
}
{
    const R = {};
    for (const [w, mode] of [[400, "bar"], [900, "rail"]]) {
        const c = new ui.Context({ width: w, height: 500, clock: "manual" });
        let sel = 0, content = null;
        const scene = (u) => {
            sel = u.scaffold("##nav", ["Home", "Search", "Me"], sel, (u) => { u.label("page " + sel); content = u.lastRect(); });
            R.box = u.lastRect(); R.i = [0, 1, 2].map((k) => u.lastRect(k));
        };
        settle(c, scene);
        if (mode === "bar") ok(R.i[0].y > content.y && Math.abs(R.i[0].y + R.i[0].height - 500) < 1, `compact: a navBar docked at the bottom (${JSON.stringify(R.i[0])})`);
        else ok(R.i[0].x < content.x && R.i[1].y > R.i[0].y, `medium/expanded: a navRail left of the content (${JSON.stringify(R.i[0])}, ${JSON.stringify(content)})`);
        click(c, scene, mid(R.i[2]));
        ok(sel === 2, `${mode}: a click on lastRect(2) selects destination 2 (${sel})`);
        c.close();
    }
}

// E. error and help lines, and the theme's error colour.
{
    const c = ctx();
    const R = {};
    let name = "";
    const scene = (u) => {
        name = u.textField("Name", name, { error: "required" }); R.err = u.lastRect();
        u.textField("Mail", "", { help: "we never share it" }); R.help = u.lastRect();
        u.textField("Plain", ""); R.plain = u.lastRect();
        u.combo("Size", ["S", "M"], 0, { error: "pick one" }); R.combo = u.lastRect();
    };
    settle(c, scene);
    ok(R.help.y - (R.err.y + R.err.height) > 8, `the error line sits between the field and the next one (${R.err.y + R.err.height} -> ${R.help.y})`);
    ok(R.plain.y - (R.help.y + R.help.height) > 8, "so does the help line");
    ok(R.err.height === R.plain.height, "lastRect() after a field with a note is the field, not the note");
    const th = c.theme();
    ok(th.error === 0xf85149ff && ui.themes.dark.error === 0xf85149ff && ui.themes.light.error === 0xcf222eff, `theme.error exists in the theme and both palettes (${th.error?.toString(16)})`);
    const px = c.pixels(), sw = c.pixelWidth;
    let red = 0;
    for (let y = Math.floor(R.err.y); y < R.help.y; y++) for (let x = Math.floor(R.err.x); x < R.err.x + R.err.width; x++) { const i = (y * sw + x) * 4; if (px[i] === 0xf8 && px[i + 1] === 0x51 && px[i + 2] === 0x49) red++; }
    ok(red > 20, `the errored field's border and note use theme.error (${red} pixels)`);
    const redIn = (r) => { let k = 0; for (let y = Math.floor(r.y); y < r.y + r.height; y++) for (let x = Math.floor(r.x); x < r.x + r.width; x++) { const i = (y * sw + x) * 4; if (px[i] === 0xf8 && px[i + 1] === 0x51 && px[i + 2] === 0x49) k++; } return k; };
    ok(redIn(R.err) > 2 * R.err.width && redIn(R.plain) === 0, `the errored field's own box is outlined in theme.error, a plain field's is not (${redIn(R.err)}, ${redIn(R.plain)})`);
    ok(redIn(R.combo) > 2 * R.combo.width, `so is a combo given { error } (${redIn(R.combo)})`);
    c.close();
}

print(`test_ui_compose: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failing`);
