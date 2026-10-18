// dyna:ui contract cases found by a black-box review: each one failed on the binary before its fix.
// A table edit commits into the cell it was started on, whatever ends it (Return, a click on
// another cell, a click outside the table); a focused table with nothing selected stays that way;
// out-of-range selections mean "none"; tagInput takes every comma-separated piece of one frame;
// timePicker steps divide the hour; drag-and-drop and range errors name what they refuse.
import * as ui from "dyna:ui";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function throwsLike(fn, cls, re, m) {
    try { fn(); ok(false, m + " (did not throw)"); }
    catch (e) { ok(e instanceof cls && re.test(e.message), `${m} (${e.constructor.name}: ${e.message})`); }
}

const W = 480, H = 400;
const ctx = () => new ui.Context({ width: W, height: H, clock: "manual" });
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
let T = 1;
const run = (c, scene) => { c.setTime((T += 0.05)); return c.frame(scene); };
const settle = (c, scene) => { run(c, scene); run(c, scene); run(c, scene); };
function click(c, scene, [x, y]) { c.pointer({ x, y, buttons: 1 }); run(c, scene); c.pointer({ x, y, buttons: 0 }); run(c, scene); }
function key(c, scene, name) { c.key(name, true); run(c, scene); c.key(name, false); run(c, scene); }

// A grid whose caller follows the documented loop: feed selected/column/edit back, write a commit
// into data[selected][column]. Every commit is logged with the cell it landed in.
function grid() {
    const data = [["a0", "b0"], ["a1", "b1"], ["a2", "b2"]];
    const st = { sel: 0, col: 0, edit: null, commits: [], R: {} };
    const scene = (u) => {
        const res = u.table("##g", {
            columns: [{ name: "A", width: 80 }, { name: "B", width: 80 }], rows: 3, visibleRows: 3,
            cell: (r, k) => data[r][k], selected: st.sel, selectedColumn: st.col,
            ...(st.edit !== null ? { edit: st.edit } : {}),
        });
        if (res.committed) { data[res.selected][res.column] = res.edit; st.commits.push(`${res.selected},${res.column}=${res.edit}`); st.edit = null; }
        else if (res.cancelled) st.edit = null;
        else if (st.edit !== null) st.edit = res.edit;
        st.sel = res.selected; st.col = res.column;
        for (let r = 0; r < 3; r++) for (let k = 0; k < 2; k++) st.R[`${r}${k}`] = u.lastRect(r, k);
        u.button("Outside"); st.R.out = u.lastRect();
    };
    return { data, st, scene };
}

// A1. a click on another cell while editing commits into the EDITED cell, then moves.
{
    const c = ctx(), { data, st, scene } = grid();
    st.edit = data[0][0];
    settle(c, scene);
    c.type("X"); run(c, scene);
    ok(st.edit === "a0X", `typing edits cell (0, 0) (${st.edit})`);
    click(c, scene, mid(st.R["21"]));
    ok(st.commits.join() === "0,0=a0X", `the click elsewhere commits the edit into (0, 0), where it began (${st.commits})`);
    ok(data[2][1] === "b2" && data[0][0] === "a0X", `cell (2, 1) is untouched (${JSON.stringify(data)})`);
    run(c, scene);
    ok(st.sel === 2 && st.col === 1, `and the selection then moves to the clicked cell (${st.sel}, ${st.col})`);
    c.close();
}

// A2. a click on another row (row hit, not a cell) mid-edit also commits into the edited cell.
{
    const c = ctx(), { data, st, scene } = grid();
    st.sel = 1; st.col = 1; st.edit = data[1][1];
    settle(c, scene);
    c.type("Y"); run(c, scene);
    click(c, scene, mid(st.R["00"]));
    run(c, scene);
    ok(st.commits.join() === "1,1=b1Y" && data[0][0] === "a0", `a click on (0, 0) commits (1, 1) and leaves (0, 0) alone (${st.commits}; ${data[0][0]})`);
    ok(st.sel === 0 && st.col === 0, `then selects (0, 0) (${st.sel}, ${st.col})`);
    c.close();
}

// A3. focus leaving the table (a click on another widget) commits; the edit never hangs open.
{
    const c = ctx(), { data, st, scene } = grid();
    st.edit = data[0][0];
    settle(c, scene);
    c.type("Z"); run(c, scene);
    click(c, scene, mid(st.R.out));
    run(c, scene);
    ok(st.edit === null && st.commits.join() === "0,0=a0Z", `a click outside the table commits the edit (${st.commits}; edit ${st.edit})`);
    ok(st.sel === 0 && st.col === 0, `and keeps the selection (${st.sel}, ${st.col})`);
    c.close();
}

// A4. Escape still cancels and Return still commits in place.
{
    const c = ctx(), { data, st, scene } = grid();
    st.edit = data[0][1]; st.col = 1;
    settle(c, scene);
    c.type("Q"); run(c, scene);
    key(c, scene, "escape");
    ok(st.edit === null && st.commits.length === 0 && data[0][1] === "b0", `Escape cancels (${st.commits}; ${data[0][1]})`);
    c.close();
}

// A5. the edited row scrolled out of view: the edit stays open (scrolling is not a commit), focus
// moving to another field meanwhile commits the caller's text, and scrolling back steals nothing.
{
    const c = ctx();
    const st = { edit: "a0", log: [], other: "", R: {} };
    let sel = 0, col = 0;
    const scene = (u) => {
        const res = u.table("##s", { columns: [{ name: "A", width: 80 }], rows: 40, visibleRows: 3, cell: (r) => `r${r}`, selected: sel, selectedColumn: col, ...(st.edit !== null ? { edit: st.edit } : {}) });
        if (res.committed) { st.log.push(`${res.selected},${res.column}=${res.edit}`); st.edit = null; }
        else if (res.cancelled) { st.log.push("cancel"); st.edit = null; }
        else if (st.edit !== null && res.edit !== undefined) st.edit = res.edit;
        sel = res.selected; col = res.column; st.R.t = u.lastRect();
        st.other = u.textField("Other", st.other); st.R.o = u.lastRect();
    };
    settle(c, scene);
    c.type("X"); run(c, scene);
    const [tx, ty] = mid(st.R.t);
    c.pointer({ x: tx, y: ty, buttons: 0, wheelY: -20 }); run(c, scene); run(c, scene);
    ok(st.log.length === 0 && st.edit === "a0X", `scrolling the edited row out of view neither commits nor cancels (${st.log}; ${st.edit})`);
    c.pointer({ x: tx, y: ty, buttons: 0, wheelY: 40 }); run(c, scene); run(c, scene);
    c.type("Y"); run(c, scene);
    ok(st.edit === "a0XY", `scrolled back, the edit still has focus (${st.edit})`);
    c.pointer({ x: tx, y: ty, buttons: 0, wheelY: -20 }); run(c, scene); run(c, scene);
    click(c, scene, mid(st.R.o));
    c.type("hello"); run(c, scene);
    ok(st.log.join() === "0,0=a0XY", `focus moving to another field while the row is out of view commits the text (${st.log})`);
    c.pointer({ x: tx, y: ty, buttons: 0, wheelY: 40 }); run(c, scene); run(c, scene);
    c.type("Z"); run(c, scene);
    ok(st.other === "helloZ", `scrolling the table back does not steal focus from the other field (${st.other})`);
    c.close();
}

// A6. the edited cell disappearing (rows shrink past it) cancels the edit instead of leaving it open.
{
    const c = ctx();
    let rows = 3, edit = "a2", log = [], sel = 2, col = 0;
    const scene = (u) => {
        const res = u.table("##v", { columns: [{ name: "A" }], rows, cell: (r) => `r${r}`, selected: sel, selectedColumn: col, ...(edit !== null ? { edit } : {}) });
        if (res.committed) { log.push("commit"); edit = null; } else if (res.cancelled) { log.push("cancel"); edit = null; } else if (edit !== null) edit = res.edit;
        sel = res.selected; col = res.column;
    };
    settle(c, scene);
    rows = 2; run(c, scene);
    ok(log.join() === "cancel" && edit === null && sel === -1, `rows shrinking past the edited cell cancels the edit (${log}; ${edit}; ${sel})`);
    c.close();
}

// B. a focused table with nothing selected does not invent a selection.
{
    const c = ctx();
    let res = null, sel = -1, col = -1, R = {};
    const scene = (u) => {
        res = u.table("##p", { columns: [{ name: "A", width: 80 }, { name: "B", width: 80 }], rows: 2, visibleRows: 5, cell: (r, k) => `${r}${k}`, selected: sel, selectedColumn: col });
        sel = res.selected; col = res.column; R.t = u.lastRect();
    };
    settle(c, scene);
    click(c, scene, [R.t.x + 40, R.t.y + R.t.height - 6]);
    settle(c, scene);
    ok(sel === -1 && col === -1, `a click on the empty body focuses but selects nothing (${sel}, ${col})`);
    key(c, scene, "right");
    ok(sel === 0 && col === 0, `an arrow key then starts at (0, 0) (${sel}, ${col})`);
    sel = 1; col = -1;
    settle(c, scene);
    ok(sel === 1 && col === -1, `a selected row with column -1 keeps column -1 (${sel}, ${col})`);
    c.close();
}

// C. out-of-range selections and sort columns mean "none".
{
    const c = ctx();
    let res = null;
    const cols = [{ name: "A" }, { name: "B" }];
    c.frame((u) => { res = u.table("##r1", { columns: cols, rows: 3, cell: () => "x", selected: 5 }); });
    ok(res.selected === -1, `selected past the last row is -1 (${res.selected})`);
    c.frame((u) => { res = u.table("##r2", { columns: cols, rows: 3, cell: () => "x", selected: 1, selectedColumn: 7 }); });
    ok(res.column === -1, `selectedColumn past the last column is -1 (${res.column})`);
    c.frame((u) => { res = u.table("##r3", { columns: cols, rows: 3, cell: () => "x", sort: { column: 9 } }); });
    ok(res.sortColumn === -1, `a sort column past the last column is -1 (${res.sortColumn})`);
    c.close();
}

// D. tagInput takes every piece of one frame's input, and Return in the same frame takes the tail.
{
    const c = ctx();
    let tags = ["x"], R = {};
    const scene = (u) => { tags = u.tagInput("Tags", tags); R.box = u.lastRect(); };
    settle(c, scene);
    click(c, scene, [R.box.x + R.box.width - 10, R.box.y + R.box.height / 2]);
    c.type("a, b ,c,d"); run(c, scene);
    ok(tags.join() === "x,a,b,c", `one paste of "a, b ,c,d" adds a, b and c in that frame (${tags})`);
    key(c, scene, "return");
    ok(tags.join() === "x,a,b,c,d", `Return adds the tail (${tags})`);
    c.type("p,q"); c.key("return", true); run(c, scene); c.key("return", false); run(c, scene);
    ok(tags.join() === "x,a,b,c,d,p,q", `pieces and the tail arriving with Return in one frame are all added (${tags})`);
    c.type("e,e,x,f,"); run(c, scene);
    ok(tags.join() === "x,a,b,c,d,p,q,e,f", `duplicates inside the paste and against the tags are dropped (${tags})`);
    c.close();
}

// E. timePicker: the step divides the hour.
{
    const c = ctx();
    c.frame((u) => {
        for (const s of [7, 9, 25, 45]) throwsLike(() => u.timePicker("t", { hour: 1, minute: 0 }, { step: s }), RangeError, /divide 60/, `timePicker step ${s} is refused`);
        for (const s of [1, 5, 15, 30, 60]) { let e = null; try { u.timePicker("t" + s, { hour: 1, minute: 0 }, { step: s }); } catch (x) { e = x; } ok(e === null, `timePicker step ${s} is accepted (${e && e.message})`); }
    });
    c.close();
}

// F. errors name what they refuse.
{
    const c = ctx();
    c.frame((u) => {
        throwsLike(() => u.accordion("##a", ["A"], 2.5, () => {}), RangeError, /accordion open/, "accordion names its open argument");
        u.label("x");
        throwsLike(() => u.lastRect(2 ** 31), RangeError, /2147483647/, "lastRect prints its limit as an integer");
    });
    c.frame((u) => {
        throwsLike(() => u.dragSource(1), TypeError, /widget drawn before/, "dragSource with no widget before it is refused");
        throwsLike(() => u.dropTarget(), TypeError, /widget drawn before/, "dropTarget with no widget before it is refused");
        u.button("b");
        throwsLike(() => u.dragSource(""), TypeError, /empty string/, "dragSource refuses the empty-string payload");
        let e = null; try { u.dragSource("ok"); u.dropTarget(); } catch (x) { e = x; }
        ok(e === null, `after a widget both are accepted (${e && e.message})`);
    });
    c.close();
}

print(`test_ui_review: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failing`);
