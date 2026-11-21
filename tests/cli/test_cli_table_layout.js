// covers: c:src/modules/term/term-module.c
// Table copies every cell's bytes into one growing arena (offsets, not one malloc + free per cell:
// malloc/free were ~25% of Table at 10K rows), computes each column's width while it reads the rows
// (the old pass walked the cell widths column by column across all rows), pads with 32-byte space
// runs and decides CSV quoting with a branch-free scan; Table was 26-32% faster at 1K-100K rows.
// Every case is checked against an independent JS renderer of the contract: widths count code
// points, "grid" pads to the widest cell (head included) and separates columns by two spaces with
// no padding after the last column, head is ruled off with dashes, "tsv" joins with tabs, "csv"
// quotes a cell holding '"', ',', CR or LF and doubles its quotes. The sweeps cross the arena's
// growth steps (0..20000 rows, cells up to 300 bytes, pads above 32) and every alignment.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_table_layout");
import { Table } from "dyna:cli";

let pass = 0, fail = 0;
const eq = (a, b, m) => {
    if (a === b) pass++;
    else {
        fail++;
        let i = 0;
        while (i < a.length && a[i] === b[i]) i++;
        print("FAIL " + m + ": first difference at " + i + ": got " + JSON.stringify(a.slice(i, i + 40)) + ", want " + JSON.stringify(b.slice(i, i + 40)));
    }
};
const width = (s) => [...s].length;
const sp = (n) => " ".repeat(Math.max(0, n));

function oracle(rows, opts = {}) {
    const fmt = opts.format || "grid";
    const head = opts.head;
    const ncols = head ? head.length : rows.length ? rows[0].length : 0;
    if (!ncols) return "";
    const cells = rows.map((r) => r.map((c) => String(c)));
    if (fmt === "tsv") return (head ? [head, ...cells] : cells).map((r) => r.join("\t") + "\n").join("");
    if (fmt === "csv") {
        const q = (s) => (/["\r\n,]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s);
        return (head ? [head, ...cells] : cells).map((r) => r.map(q).join(",") + "\n").join("");
    }
    const align = opts.align || [];
    const w = [];
    for (let j = 0; j < ncols; j++) {
        w[j] = head ? width(head[j]) : 0;
        for (const r of cells) w[j] = Math.max(w[j], width(r[j]));
    }
    const line = (r, isHead) => r.map((c, j) => {
        const pad = w[j] - width(c), a = align[j] || "left";
        let pl, pr;
        if (a === "center") { pl = Math.floor(pad / 2); pr = pad - pl; }
        else if (a === "right") { pl = pad; pr = 0; }
        else { pl = 0; pr = pad; }
        return sp(pl) + c + (j + 1 < ncols ? sp(pr) + "  " : "");
    }).join("") + "\n";
    let out = "";
    if (head) out += line(head, true) + w.map((x) => "-".repeat(x)).join("  ") + "\n";
    for (const r of cells) out += line(r, false);
    return out;
}

const WORDS = ["", "a", "é", "日本", "x,y", "say \"hi\"", "line\nbreak", "cr\rhere", "tab\there", "🎉ok"];
function rowsOf(n, ncols, seed) {
    const rows = [];
    let s = seed >>> 0;
    const rnd = () => (s = (s * 1103515245 + 12345) >>> 0) / 4294967296;
    for (let i = 0; i < n; i++) {
        const r = [];
        for (let j = 0; j < ncols; j++) {
            const k = Math.floor(rnd() * 14);
            if (k < WORDS.length) r.push(WORDS[k] + (k & 1 ? i : ""));
            else if (k === 10) r.push(i * (j + 1));
            else if (k === 11) r.push((i / 7).toFixed(3) * 1);
            else if (k === 12) r.push("w".repeat(Math.floor(rnd() * 300)));
            else r.push(sp(Math.floor(rnd() * 70)) + "|");
        }
        rows.push(r);
    }
    return rows;
}

__kitS.section("grid with every alignment, head and no head, across arena growth", () => {
    for (const n of [0, 1, 2, 3, 17, 255, 1000, 20000])
        for (const ncols of [1, 2, 5]) {
            const rows = rowsOf(n, ncols, n * 31 + ncols);
            const head = Array.from({ length: ncols }, (_, j) => "h" + "e".repeat(j * 9) + j);
            const al = ["left", "right", "center", "center", "right"].slice(0, ncols);
            eq(Table(rows, { head, align: al }), oracle(rows, { head, align: al }), "grid n=" + n + " c=" + ncols);
            if (n) eq(Table(rows), oracle(rows), "grid no head n=" + n + " c=" + ncols);
            eq(Table(rows, { head, align: al.slice(0, 1) }), oracle(rows, { head, align: al.slice(0, 1) }), "partial align n=" + n);
        }
});

__kitS.section("head wider than every cell, pads longer than one 32-byte space run", () => {
    const rows = [["a", 1], ["bb", 22]];
    for (const hw of [1, 31, 32, 33, 64, 65, 100]) {
        const head = ["x".repeat(hw), "y".repeat(hw)];
        for (const al of [["left", "left"], ["right", "right"], ["center", "center"]])
            eq(Table(rows, { head, align: al }), oracle(rows, { head, align: al }), "wide head " + hw + " " + al[0]);
    }
});

__kitS.section("tsv and csv quoting", () => {
    for (const n of [1, 9, 500, 5000]) {
        const rows = rowsOf(n, 4, n);
        const head = ["id", "a,b", "q\"uote", "plain"];
        for (const format of ["tsv", "csv"]) {
            eq(Table(rows, { head, format }), oracle(rows, { head, format }), format + " n=" + n);
            eq(Table(rows, { format }), oracle(rows, { format }), format + " no head n=" + n);
        }
    }
    eq(Table([["\"", ",", "\r", "\n", "x"]], { format: "csv" }), "\"\"\"\",\",\",\"\r\",\"\n\",x\n", "each quoting trigger alone");
});

__kitS.end();
print(`test_cli_table_layout: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
