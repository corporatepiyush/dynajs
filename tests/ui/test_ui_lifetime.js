// flags: --std
// dyna:ui object lifetimes. A Context keeps object templates for the rects lastRect() returns and the
// object table() returns; their shapes reference the realm's Object.prototype, so the Context's
// gc_mark must report them or a Context that is never closed keeps its own realm alive and the
// runtime asserts at teardown (exit 134). That only shows in a child process that exits with the
// Context still open, so the child is the oracle. The in-process half drives several live Contexts
// through collections while their results are still referenced.
import * as ui from "dyna:ui";
import * as os from "os";
import * as std from "std";

let pass = 0, fail = 0, skipped = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }

const BIN = std.getenv("DYNAJS_TEST_BIN") || "./dynajs";
const TBASE = (std.getenv("TMPDIR") || "/tmp/").replace(/\/?$/, "/") + "dyna_ui_lifetime_" + os.getpid() + "_";

function child(src) {
    const path = TBASE + "child.js", out = TBASE + "child.out";
    const f = std.open(path, "w");
    f.puts(src);
    f.close();
    const fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC);
    const pid = os.exec([BIN, "--std", path], { block: false, stdout: fd, stderr: fd });
    os.close(fd);
    const deadline = Date.now() + 60000;
    let r;
    for (;;) {
        r = os.waitpid(pid, os.WNOHANG);
        if (r[0] !== 0) break;
        if (Date.now() > deadline) { os.kill(pid, 9); os.waitpid(pid, 0); return { code: -1, text: "timed out" }; }
        os.sleep(10);
    }
    let text = "";
    try { text = String(std.loadFile(out)); } catch (e) { text = ""; }
    os.remove(path);
    os.remove(out);
    return { code: r[1] & 0x7f ? 128 + (r[1] & 0x7f) : r[1] >> 8, text };
}

const OPEN_CONTEXTS = `import * as ui from "dyna:ui";
const cs = [0, 1, 2].map(() => new ui.Context({ width: 200, height: 100, clock: "manual" }));
let r = null, t = null;
for (const c of cs) for (let i = 0; i < 3; i++) c.frame((u) => { u.button("x"); r = u.lastRect(); t = u.table("t", { columns: [{ name: "a" }], rows: 2, cell: (k) => "v" + k }); });
print(JSON.stringify(r), JSON.stringify(t));
`;

if (typeof os.exec !== "function") { skipped++; print("SKIP: os.exec is not available"); }
else {
    const a = child(OPEN_CONTEXTS);
    ok(a.code === 0 && a.text.includes('"width":19'), `a script that exits with Contexts still open (after lastRect() and table()) exits cleanly (code ${a.code}: ${a.text.trim().slice(0, 160)})`);
    const b = child(OPEN_CONTEXTS + `throw new Error("boom");\n`);
    ok(b.code === 1 && b.text.includes("boom"), `one that throws with them open exits 1 with the error, not an abort at teardown (code ${b.code})`);
}

{
    const c = new ui.Context({ width: 200, height: 100, clock: "manual" });
    let r = null, t = null;
    for (let i = 0; i < 3; i++) c.frame((u) => { u.button("x"); r = u.lastRect(); t = u.table("t", { columns: [{ name: "a" }], rows: 2, cell: (k) => "v" + k, selected: 1 }); });
    ok(JSON.stringify(Object.keys(r)) === '["x","y","width","height"]' && r.width > 0 && r.height > 0 && Object.getPrototypeOf(r) === Object.prototype,
        `lastRect() is a plain object with x, y, width, height in that order (${JSON.stringify(r)})`);
    ok(JSON.stringify(Object.keys(t)) === '["selected","sortColumn","descending","column","activated","edit","committed","cancelled"]' && t.selected === 1 && t.sortColumn === -1 && t.descending === false && t.column === -1 && t.edit === undefined,
        `table() returns { selected, sortColumn, descending, column, activated, edit, committed, cancelled } in that order (${JSON.stringify(t)})`);
    r.extra = 1;
    delete r.x;
    const r2 = c.lastRect();
    ok(r2 && !("extra" in r2) && "x" in r2, "changing one returned rect does not change the next");
    c.close();
}

{
    let keep = [], made = 0, rects = 0;
    for (let round = 0; round < 30; round++) {
        const cs = Array.from({ length: 6 }, () => new ui.Context({ width: 160, height: 90, clock: "manual" }));
        for (const c of cs)
            for (let f = 0; f < 3; f++)
                c.frame((u) => {
                    u.button("b");
                    const r = u.lastRect();
                    if (r) { rects++; keep.push(r, { r, c }); }
                    keep.push(u.table("t", { columns: [{ name: "a" }], rows: 3, cell: (i) => "x" + i }));
                });
        made += cs.length;
        if (round % 3 === 0) cs.forEach((c) => c.close());
        if (keep.length > 300) keep = keep.slice(150);
        std.gc();
    }
    const last = keep.filter((v) => v && "x" in v).pop();
    ok(rects === made * 2 && last && last.width > 0, `${made} Contexts, ${rects} rects kept across collections, some closed and some dropped open, stay intact (${JSON.stringify(last)})`);
}

print(`test_ui_lifetime: ${pass} pass, ${fail} fail${skipped ? `, ${skipped} skipped` : ""}`);
if (fail) throw new Error(`${fail} failures`);
