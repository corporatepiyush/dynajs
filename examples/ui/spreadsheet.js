// Spreadsheet — a 500-row sheet on dyna:ui: a virtualised grid, a formula bar, recalculation, undo and a live chart.
// apps:needs dyna:ui
// flags: --std
//
// WHAT IT IS
//   Eight columns (A..H) by 500 rows in a virtualised table: only the rows on screen are asked for
//   their cells, so the grid costs the same at 500 rows as at 50. The formula bar holds the address
//   of the cell being edited and its source; Return commits it. Cells are selected in the grid itself
//   (click, arrows, Home/End) and Return or a double click edits one in place: Return commits, Escape
//   cancels. Formulas are =A1+B2*3, ranges in
//   SUM/MIN/MAX/AVG (=SUM(B1:B20)) and parentheses; every commit recomputes what depends on it, a
//   reference cycle shows #CYCLE instead of looping, and Undo walks the edit history back. A split
//   pane beside the grid charts the column chosen in the segmented control as bars and a line.
//   Clicking a header sorts the view (not the cells: a formula keeps naming the same cells).
//   With no arguments it drives itself: every press lands on a widget found with lastRect(), and
//   every value is checked against a second, independent evaluation of the same sheet.
//
// MODULES  dyna:ui (Context, table cell selection and in-cell edit, lastRect, textField, split, segmented, bars, plot, toolbar, toast) · dyna:sdl (only with --window)
//
// RUN      dynajs --std examples/ui/spreadsheet.js             drives itself headlessly and checks the outcome
//          dynajs --std examples/ui/spreadsheet.js --window    opens it in a window (needs CONFIG_SDL3)

import * as ui from "dyna:ui";

const COLS = 8,
    ROWS = 500,
    NAMES = "ABCDEFGH";
const src = new Map();
for (let r = 0; r < ROWS; r++) {
    src.set("A" + (r + 1), "item " + (r + 1));
    src.set("B" + (r + 1), String(((r * 37) % 101) + 1));
    src.set("C" + (r + 1), String(((r * 13) % 17) + 2));
}
const history = [];
const state = {
    addr: "D1",
    text: "",
    selected: 0,
    col: 4,
    editing: null,
    sort: { column: -1, descending: false },
    chart: 1,
    ratio: 0.72,
    undone: 0,
    cellCalls: 0,
};

// ---- the evaluator: a small recursive-descent parser over one formula, memoised per recalculation ----
const parseAddr = (a) => {
    const m = /^([A-H])([1-9][0-9]*)$/.exec(a);
    return m && +m[2] <= ROWS ? { c: NAMES.indexOf(m[1]), r: +m[2] - 1 } : null;
};
let cache = new Map();
function value(addr, stack = new Set()) {
    if (cache.has(addr)) return cache.get(addr);
    if (stack.has(addr)) return "#CYCLE";
    const s = src.get(addr) ?? "";
    let v;
    if (!s.startsWith("=")) v = s === "" ? "" : isFinite(+s) ? +s : s;
    else {
        stack.add(addr);
        try {
            v = formula(s.slice(1), stack);
        } catch (e) {
            v = e.message.startsWith("#") ? e.message : "#ERROR";
        }
        stack.delete(addr);
    }
    cache.set(addr, v);
    return v;
}
function formula(text, stack) {
    const toks = text.match(/[A-H][0-9]+:[A-H][0-9]+|[A-Z]+\(|[A-H][0-9]+|[0-9.]+|[-+*/()]|\S/g) ?? [];
    let i = 0;
    const num = (v) => {
        if (typeof v === "string" && v.startsWith("#")) throw new Error(v);
        return typeof v === "number" ? v : 0;
    };
    const range = (t) => {
        const [a, b] = t.split(":").map(parseAddr);
        if (!a || !b) throw new Error("#REF");
        const out = [];
        for (let c = Math.min(a.c, b.c); c <= Math.max(a.c, b.c); c++)
            for (let r = Math.min(a.r, b.r); r <= Math.max(a.r, b.r); r++)
                out.push(num(value(NAMES[c] + (r + 1), stack)));
        return out;
    };
    const atom = () => {
        const t = toks[i++];
        if (t === undefined) throw new Error("#ERROR");
        if (t === "(") {
            const v = sum();
            if (toks[i++] !== ")") throw new Error("#ERROR");
            return v;
        }
        if (t === "-") return -atom();
        if (/^[A-Z]+\($/.test(t)) {
            const vs = range(toks[i++] ?? "");
            if (toks[i++] !== ")") throw new Error("#ERROR");
            const fn = t.slice(0, -1);
            if (fn === "SUM") return vs.reduce((a, b) => a + b, 0);
            if (fn === "MIN") return Math.min(...vs);
            if (fn === "MAX") return Math.max(...vs);
            if (fn === "AVG") return vs.reduce((a, b) => a + b, 0) / vs.length;
            throw new Error("#NAME");
        }
        if (/^[A-H][0-9]+$/.test(t)) {
            if (!parseAddr(t)) throw new Error("#REF");
            return num(value(t, stack));
        }
        if (/^[0-9.]+$/.test(t) && isFinite(+t)) return +t;
        throw new Error("#ERROR");
    };
    const product = () => {
        let v = atom();
        while (toks[i] === "*" || toks[i] === "/") v = toks[i++] === "*" ? v * atom() : v / atom();
        return v;
    };
    const sum = () => {
        let v = product();
        while (toks[i] === "+" || toks[i] === "-") v = toks[i++] === "+" ? v + product() : v - product();
        return v;
    };
    const v = sum();
    if (i !== toks.length) throw new Error("#ERROR");
    return v;
}
const show = (v) => (typeof v === "number" ? String(Math.round(v * 1000) / 1000) : v);

function commit(addr, text) {
    const before = src.get(addr) ?? "";
    if (before === text) return;
    history.push({ addr, before });
    if (text === "") src.delete(addr);
    else src.set(addr, text);
    cache = new Map();
}
function undo() {
    const h = history.pop();
    if (!h) return;
    if (h.before === "") src.delete(h.addr);
    else src.set(h.addr, h.before);
    cache = new Map();
    state.undone++;
}
function order() {
    const idx = Array.from({ length: ROWS }, (_, r) => r);
    const c = state.sort.column;
    if (c < 1) return idx;
    const key = (r) => value(NAMES[c - 1] + (r + 1));
    const cmp = (a, b) => {
        const x = key(a),
            y = key(b);
        const d = typeof x === "number" && typeof y === "number" ? x - y : String(x).localeCompare(String(y));
        return d || a - b;
    };
    idx.sort(state.sort.descending ? (a, b) => cmp(b, a) : cmp);
    return idx;
}

const rects = {};
let view = order();
function app(u) {
    const rec = (name) => {
        rects[name] = u.lastRect();
    };
    u.toolbar(() => {
        state.addr = u.textField("##addr", state.addr, { width: 60 }).toUpperCase();
        rec("addr");
        if (u.submitted) {
            const a = parseAddr(state.addr);
            if (a) {
                state.selected = view.indexOf(a.r);
                state.col = a.c + 1;
                state.text = src.get(state.addr) ?? "";
            }
        }
        u.label("fx");
        state.text = u.textField("##fx", state.text, { width: 360, hint: "a value or =formula" });
        rec("fx");
        if (u.submitted && parseAddr(state.addr)) {
            commit(state.addr, state.text);
            view = order();
        }
        u.separator();
        if (u.button("Undo")) {
            undo();
            view = order();
            state.text = src.get(state.addr) ?? "";
        }
        rec("undo");
    });
    state.ratio = u.split(
        "sheet",
        state.ratio,
        () => {
            const t = u.table("grid", {
                columns: [
                    { name: "#", width: 44 },
                    ...[...NAMES].map((n) => ({ name: n, width: n === "A" ? 110 : 80 })),
                ],
                rows: ROWS,
                cell: (row, col) => {
                    state.cellCalls++;
                    const r = view[row];
                    return col === 0 ? String(r + 1) : show(value(NAMES[col - 1] + (r + 1)));
                },
                visibleRows: 18,
                selected: state.selected,
                selectedColumn: state.col,
                ...(state.editing !== null ? { edit: state.editing } : {}),
                sort: state.sort,
            });
            rec("grid");
            rects.headers = [...Array(COLS + 1).keys()].map((k) => u.lastRect(-1, k));
            rects.cells = new Map();
            for (let row = 0; row < ROWS; row++) {
                if (!u.lastRect(row, 0)) {
                    if (rects.cells.size) break;
                    continue;
                }
                for (let k = 0; k <= COLS; k++) rects.cells.set(row + ":" + k, u.lastRect(row, k));
            }
            const addrOf = (row, k) => NAMES[Math.max(k, 1) - 1] + (view[row] + 1);
            if (t.sortColumn !== state.sort.column || t.descending !== state.sort.descending) {
                state.sort = { column: t.sortColumn, descending: t.descending };
                view = order();
            }
            if (state.editing !== null) {
                if (t.edit !== undefined) state.editing = t.edit;
                if (t.committed) {
                    commit(addrOf(t.selected, t.column), state.editing);
                    view = order();
                    state.text = src.get(state.addr) ?? "";
                }
                if (t.committed || t.cancelled) state.editing = null;
            }
            if ((t.selected !== state.selected || t.column !== state.col) && t.selected >= 0) {
                state.selected = t.selected;
                state.col = t.column;
                state.addr = addrOf(t.selected, t.column);
                state.text = src.get(state.addr) ?? "";
            }
            if (t.activated && state.editing === null) state.editing = src.get(state.addr) ?? "";
        },
        () => {
            state.chart = u.segmented("##chart", ["B", "C", "D"], state.chart - 1) + 1;
            rec("chart");
            rects.chartItems = [0, 1, 2].map((k) => u.lastRect(k));
            const col = NAMES[state.chart];
            const vals = new Float64Array(40);
            for (let r = 0; r < 40; r++) {
                const v = value(col + (r + 1));
                vals[r] = typeof v === "number" ? v : 0;
            }
            u.heading("Column " + col + ", rows 1-40");
            u.bars("##bars", vals, { height: 120 });
            u.plot("##line", vals, { height: 80 });
            u.label("sum " + show(vals.reduce((a, b) => a + b, 0)));
        },
    );
    u.statusbar(() => {
        u.label(state.addr + " = " + show(value(state.addr) ?? ""));
        u.spacer();
        u.label(`${src.size} cells, ${history.length} edits`);
    });
}

// ---- headless driver ----
const W = 1280,
    H = 720;
const c = new ui.Context({ width: W, height: H, clock: "manual" });
let now = 0;
const frame = () => {
    now += 1 / 60;
    c.setTime(now);
    return c.frame(app);
};
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
const press = ([x, y]) => {
    c.pointer({ x, y, buttons: 1 });
    frame();
    c.pointer({ x, y, buttons: 0 });
    frame();
    frame();
};
const enter = (field, text) => {
    press(mid(rects[field]));
    c.key("a", true, { ctrl: true });
    c.key("a", false, { ctrl: true });
    c.key("backspace");
    c.type(text);
    frame();
    c.key("return");
    frame();
    frame();
};
// The reference evaluator: a direct translation of the same grammar with no cache and no shared code
// path, so a wrong memo or a missed invalidation in the app shows up as a disagreement.
function ref(addr, seen = []) {
    if (seen.includes(addr)) return "#CYCLE";
    const s = src.get(addr) ?? "";
    if (!s.startsWith("=")) return s === "" ? "" : isFinite(+s) ? +s : s;
    const body = s.slice(1);
    const cells = (a, b) => {
        const p = parseAddr(a),
            q = parseAddr(b),
            out = [];
        for (let c = p.c; c <= q.c; c++) for (let r = p.r; r <= q.r; r++) out.push(NAMES[c] + (r + 1));
        return out;
    };
    const nums = (list) => {
        const vs = list.map((x) => ref(x, [...seen, addr]));
        const bad = vs.find((v) => typeof v === "string" && v.startsWith("#"));
        if (bad) throw bad;
        return vs.map((v) => (typeof v === "number" ? v : 0));
    };
    try {
        const js = body
            .replace(/(SUM|MIN|MAX|AVG)\(([A-H][0-9]+):([A-H][0-9]+)\)/g, (_, f, a, b) => {
                const vs = nums(cells(a, b));
                return (
                    "(" +
                    (f === "SUM"
                        ? vs.reduce((x, y) => x + y, 0)
                        : f === "MIN"
                          ? Math.min(...vs)
                          : f === "MAX"
                            ? Math.max(...vs)
                            : vs.reduce((x, y) => x + y, 0) / vs.length) +
                    ")"
                );
            })
            .replace(/[A-H][0-9]+/g, (a) => "(" + nums([a])[0] + ")");
        if (!/^[-+*/().0-9\s]+$/.test(js)) return "#ERROR";
        return new Function("return " + js)();
    } catch (e) {
        return typeof e === "string" ? e : "#ERROR";
    }
}
const agrees = (addr) => {
    cache = new Map();
    const a = value(addr),
        b = ref(addr);
    return (
        a === b ||
        (typeof a === "number" && typeof b === "number" && Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(b)))
    );
};

const results = {};
if (scriptArgs.includes("--window")) await runWindow();
else {
    for (let k = 0; k < 3; k++) frame();
    state.cellCalls = 0;
    frame();
    const perFrame = state.cellCalls;
    results["the grid asks only for the cells on screen, not all 4500"] = perFrame > 0 && perFrame <= 30 * (COLS + 1);

    enter("addr", "d1");
    results["typing an address selects that cell and loads its source"] =
        state.addr === "D1" && state.selected === 0 && state.text === "";
    enter("fx", "=SUM(B1:B10)");
    const want = Array.from({ length: 10 }, (_, r) => ((r * 37) % 101) + 1).reduce((a, b) => a + b, 0);
    results["Return commits a SUM over a range, and it evaluates to the plain-JS total"] =
        value("D1") === want && agrees("D1");

    enter("addr", "D2");
    enter("fx", "=D1*2+(C1-B1)/4");
    results["a formula over a formula agrees with the reference evaluator"] = agrees("D2");
    enter("addr", "B1");
    enter("fx", "1000");
    results["changing an input recomputes everything that depends on it"] =
        value("D1") === want - 1 + 1000 && agrees("D1") && agrees("D2");

    enter("addr", "E1");
    enter("fx", "=E2+1");
    enter("addr", "E2");
    enter("fx", "=E1+1");
    results["a reference cycle reads #CYCLE instead of looping"] = value("E1") === "#CYCLE" && value("E2") === "#CYCLE";
    enter("addr", "E3");
    enter("fx", "=E1*2");
    results["and a cell that reads a cycle reports it too"] = value("E3") === "#CYCLE";
    enter("addr", "F1");
    enter("fx", "=SUM(B1:B10");
    results["a malformed formula is an error, not an exception"] = value("F1") === "#ERROR";

    const edits = history.length;
    for (let k = 0; k < 4; k++) press(mid(rects.undo));
    results["Undo walks four edits back"] = history.length === edits - 4 && state.undone === 4;
    results["leaving the earlier cells as they were"] =
        !src.has("E2") && !src.has("E3") && !src.has("F1") && src.get("E1") === undefined && src.get("B1") === "1000";

    press(mid(rects.headers[2]));
    const col = (r) => value("B" + (r + 1));
    results["clicking the B header sorts the view ascending by B"] =
        state.sort.column === 2 &&
        !state.sort.descending &&
        view.every((r, i) => i === 0 || col(view[i - 1]) <= col(r));
    press(mid(rects.headers[2]));
    results["a second click sorts it descending"] =
        state.sort.descending && view.every((r, i) => i === 0 || col(view[i - 1]) >= col(r));
    results["sorting reorders the view only: no cell changed"] = agrees("D1") && src.get("B1") === "1000";

    press(mid(rects.cells.get("2:1")));
    results["clicking a cell selects it in the sorted order"] =
        state.selected === 2 && state.col === 1 && state.addr === "A" + (view[2] + 1);
    c.key("right");
    frame();
    c.key("right", false);
    c.key("right");
    frame();
    c.key("right", false);
    c.key("down");
    frame();
    c.key("down", false);
    frame();
    results["arrow keys move the cell, and the formula bar follows"] =
        state.selected === 3 &&
        state.col === 3 &&
        state.addr === "C" + (view[3] + 1) &&
        state.text === (src.get(state.addr) ?? "");

    const target = "G" + (view[3] + 1);
    for (let k = 0; k < 4; k++) {
        c.key("right");
        frame();
        c.key("right", false);
    }
    c.key("return");
    frame();
    c.key("return", false);
    frame();
    results["Return on a cell edits it in place"] = state.editing === "" && state.addr === target;
    c.type("=B1+1");
    frame();
    c.key("return");
    frame();
    c.key("return", false);
    frame();
    frame();
    results["and Return commits what was typed there"] =
        state.editing === null && src.get(target) === "=B1+1" && value(target) === 1001 && agrees(target);
    const g1 = rects.cells.get("3:7");
    c.pointer({ x: g1.x + 10, y: g1.y + g1.height / 2, buttons: 1, time: now });
    frame();
    c.pointer({ x: g1.x + 10, y: g1.y + g1.height / 2, buttons: 0, time: now });
    frame();
    c.pointer({ x: g1.x + 10, y: g1.y + g1.height / 2, buttons: 1, time: now });
    frame();
    c.pointer({ x: g1.x + 10, y: g1.y + g1.height / 2, buttons: 0, time: now });
    frame();
    results["a double click edits the cell holding its source"] = state.editing === "=B1+1";
    c.type("*5");
    frame();
    c.key("escape");
    frame();
    c.key("escape", false);
    frame();
    results["Escape cancels the edit: the cell keeps its formula"] =
        state.editing === null && src.get(target) === "=B1+1";

    press(mid(rects.chartItems[2]));
    results["the segmented control switches the chart to column D"] = state.chart === 3;

    const before = c.stats().reusedFrames;
    for (let k = 0; k < 30; k++) {
        c.setTime(now);
        c.frame(app);
    }
    results["an idle sheet reuses its last layout"] = c.stats().reusedFrames - before >= 29;
    c.close();
    check(results);
    print(
        `spreadsheet: ${Object.keys(results).length} checks passed (${perFrame} cell reads per frame for ${ROWS * (COLS + 1)} cells)`,
    );
}

async function runWindow() {
    let sdl;
    try {
        sdl = await import("dyna:sdl");
    } catch (e) {
        throw new Error("--window needs dyna:sdl (build with CONFIG_SDL3=y)");
    }
    sdl.init(["video"]);
    const win = new sdl.Window("Spreadsheet", W, H, { resizable: true, highDensity: true });
    const r = new sdl.Renderer(win);
    const w = new ui.Context({ target: r });
    for (let running = true; running;) {
        for (const e of sdl.pollEvents()) if (e.type === "quit") running = false;
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
