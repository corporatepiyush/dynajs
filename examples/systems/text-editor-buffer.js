// Text editor buffer — a block rope with Fenwick-tree offset and line indexes, undo/redo, search across block
// boundaries and operational-transform merging of concurrent edits, checked against a plain string.
//
// WHAT IT IS
//   The data structure under an editor that must stay fast on a 10 MB file:
//
//   rope        text in blocks of at most 2 KiB; a Fenwick tree over block lengths turns "character offset -> block"
//               into a log-time prefix search, a second Fenwick over newline counts does "line -> offset"; a block
//               that grows past 2 KiB splits and both trees are rebuilt (rare, amortised)
//   history     every edit pushes its inverse onto an undo Deque; undo moves it to redo; a new edit clears redo
//   search      a Matcher (Boyer-Moore-Horspool) over a sliding window that overlaps block boundaries, so a match
//               split across two blocks is still found
//   collaborate two replicas edit concurrently; operations are transformed against each other (insert/insert
//               tie-broken by site id, insert/delete and delete/delete with range overlap) and both converge
//   oracle      every edit is mirrored on a plain JS string; content, line lookups, undo/redo and search results
//               must match it; DiffChars between snapshots shows exactly the edit that was made
//
// MODULES  dyna:structures (Fenwick, Deque) · dyna:matcher (Matcher, DiffChars) · dyna:random · dyna:cli ·
//          String extensions
//
// RUN      dynajs examples/systems/text-editor-buffer.js

import { Fenwick, Deque } from "dyna:structures";
import { Matcher, DiffChars } from "dyna:matcher";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1976);
const MAX_BLOCK = 2048;

// =============================================================================
// Block rope
// =============================================================================
class Rope {
    constructor(text = "") {
        this.blocks = [];
        for (let i = 0; i < text.length; i += MAX_BLOCK / 2) this.blocks.push(text.slice(i, i + MAX_BLOCK / 2));
        if (!this.blocks.length) this.blocks.push("");
        this.rebuild();
        this.undo = new Deque();
        this.redo = new Deque();
        this.rebuilds = 0;
    }
    rebuild() {
        this.cap = this.blocks.length + 1;
        this.len = new Fenwick(this.cap);
        this.nl = new Fenwick(this.cap);
        this.blocks.forEach((b, i) => {
            this.len.update(i, b.length);
            this.nl.update(i, b.count("\n"));
        });
        this.rebuilds = (this.rebuilds ?? 0) + 1;
    }
    get length() {
        return this.len.prefixSum(this.blocks.length - 1);
    }
    // First block whose cumulative length exceeds pos (or the last block for pos == length).
    locate(pos) {
        let lo = 0,
            hi = this.blocks.length - 1;
        while (lo < hi) {
            const mid = (lo + hi) >> 1;
            if (this.len.prefixSum(mid) > pos) hi = mid;
            else lo = mid + 1;
        }
        const before = lo ? this.len.prefixSum(lo - 1) : 0;
        return [lo, pos - before];
    }
    rawInsert(pos, text) {
        const [b, off] = this.locate(pos);
        const s = this.blocks[b];
        this.blocks[b] = s.slice(0, off) + text + s.slice(off);
        if (this.blocks[b].length > MAX_BLOCK) {
            const big = this.blocks[b],
                parts = [];
            for (let i = 0; i < big.length; i += MAX_BLOCK / 2) parts.push(big.slice(i, i + MAX_BLOCK / 2));
            this.blocks.splice(b, 1, ...parts);
            this.rebuild();
        } else {
            this.len.update(b, text.length);
            this.nl.update(b, text.count("\n"));
        }
    }
    rawDelete(pos, n) {
        let removed = "";
        while (n > 0) {
            const [b, off] = this.locate(pos);
            const s = this.blocks[b],
                take = Math.min(n, s.length - off);
            const cut = s.slice(off, off + take);
            this.blocks[b] = s.slice(0, off) + s.slice(off + take);
            this.len.update(b, -take);
            this.nl.update(b, -cut.count("\n"));
            removed += cut;
            n -= take;
        }
        return removed;
    }
    insert(pos, text) {
        this.rawInsert(pos, text);
        this.undo.pushBack({ op: "del", pos, n: text.length });
        this.redo = new Deque();
    }
    delete(pos, n) {
        const t = this.rawDelete(pos, n);
        this.undo.pushBack({ op: "ins", pos, text: t });
        this.redo = new Deque();
        return t;
    }
    apply(inv, to) {
        if (inv.op === "del") {
            const t = this.rawDelete(inv.pos, inv.n);
            to.pushBack({ op: "ins", pos: inv.pos, text: t });
        } else {
            this.rawInsert(inv.pos, inv.text);
            to.pushBack({ op: "del", pos: inv.pos, n: inv.text.length });
        }
    }
    undoOne() {
        const inv = this.undo.popBack();
        if (inv) this.apply(inv, this.redo);
        return !!inv;
    }
    redoOne() {
        const inv = this.redo.popBack();
        if (inv) this.apply(inv, this.undo);
        return !!inv;
    }
    text() {
        return this.blocks.join("");
    }
    slice(from, to) {
        let out = "",
            pos = from;
        while (pos < to) {
            const [b, off] = this.locate(pos);
            const piece = this.blocks[b].slice(off, off + (to - pos));
            if (!piece) break;
            out += piece;
            pos += piece.length;
        }
        return out;
    }
    lineStart(line) {
        if (line === 0) return 0;
        let lo = 0,
            hi = this.blocks.length - 1;
        while (lo < hi) {
            const mid = (lo + hi) >> 1;
            if (this.nl.prefixSum(mid) >= line) hi = mid;
            else lo = mid + 1;
        }
        let need = line - (lo ? this.nl.prefixSum(lo - 1) : 0),
            base = lo ? this.len.prefixSum(lo - 1) : 0;
        const s = this.blocks[lo];
        for (let i = 0; i < s.length; i++) if (s[i] === "\n" && --need === 0) return base + i + 1;
        return -1;
    }
    find(pattern) {
        const m = new Matcher(pattern),
            hits = [];
        const total = this.length,
            W = 64 * 1024;
        for (let start = 0; start < total; start += W) {
            const window = this.slice(start, Math.min(total, start + W + pattern.length - 1));
            for (const i of m.allIn(window)) if (start + i < Math.min(total, start + W)) hits.push(start + i);
        }
        return hits;
    }
}

// =============================================================================
// Random editing session mirrored on a plain string
// =============================================================================
const words = "the quick brown fox jumps over lazy dog lorem ipsum dolor sit amet buffer rope editor".split(" ");
const para = () =>
    Array.from({ length: 40 + rng.nextBounded(40) }, () => words[rng.nextBounded(words.length)]).join(" ") + "\n";
let initial = "";
while (initial.length < 400_000) initial += para();
const rope = new Rope(initial);
let mirror = initial;
const t0 = performance.now();
let edits = 0;
for (let i = 0; i < 20_000; i++) {
    const r = rng.nextFloat(),
        pos = rng.nextBounded(mirror.length + 1);
    if (r < 0.55) {
        const t = rng.nextFloat() < 0.1 ? para() : words[rng.nextBounded(words.length)] + " ";
        rope.insert(pos, t);
        mirror = mirror.slice(0, pos) + t + mirror.slice(pos);
    } else if (r < 0.9) {
        const n = Math.min(1 + rng.nextBounded(30), mirror.length - pos);
        if (n > 0) {
            rope.delete(pos, n);
            mirror = mirror.slice(0, pos) + mirror.slice(pos + n);
        }
    } else {
        const ok = rope.undoOne();
        if (ok) mirror = rope.text();
    }
    edits++;
}
const editMs = performance.now() - t0;
results["rope content equals the mirrored string after 20 000 edits"] = rope.text() === mirror;
results["length from the Fenwick tree equals the string length"] = rope.length === mirror.length;
results["no block exceeds the size limit"] = rope.blocks.every((b) => b.length <= MAX_BLOCK);
{
    const lines = mirror.split("\n");
    let ok = true,
        at = 0;
    for (let l = 0; l < lines.length; l += 37) {
        const expected = lines.slice(0, l).reduce((s, x) => s + x.length + 1, 0);
        if (rope.lineStart(l) !== expected) ok = false;
        at = l;
    }
    results["line -> offset lookups match the string"] = ok && at > 100;
}
{
    const target = "lazy dog lorem";
    const want = mirror.indexOfAll(target);
    results["search finds every occurrence, including across block boundaries"] =
        JSON.stringify(rope.find(target)) === JSON.stringify(want) && want.length > 10;
    const boundary = rope.blocks.slice(0, 3).reduce((s, b) => s + b.length, 0);
    const straddle = rope.slice(boundary - 4, boundary + 4);
    results["a match straddling a block boundary is found"] = rope.find(straddle).includes(boundary - 4);
}
{
    const before = rope.text();
    rope.insert(1000, "<<INSERTED>>");
    const after = rope.text();
    const changes = DiffChars(before, after).filter((h) => h.op !== 0);
    results["DiffChars shows exactly the insertion"] =
        changes.length === 1 && changes[0].op === 1 && changes[0].text.length === 12;
    rope.undoOne();
    results["undo restores the previous text"] = rope.text() === before;
    rope.redoOne();
    results["redo re-applies it"] = rope.text() === after;
    rope.undoOne();
    rope.insert(5, "x");
    results["a new edit clears the redo stack"] = !rope.redoOne();
}
{
    const r = new Rope("abc");
    let s = "abc";
    for (let i = 0; i < 50; i++) {
        r.insert(i % 3, String(i));
        s = s.slice(0, i % 3) + i + s.slice(i % 3);
    }
    let undone = 0;
    while (r.undoOne()) undone++;
    results["undoing everything returns to the original"] = r.text() === "abc" && undone === 50;
}

// =============================================================================
// Operational transform: two sites, concurrent operations, convergence
// =============================================================================
function transform(a, b) {
    if (a.type === "ins" && b.type === "ins")
        return b.pos < a.pos || (b.pos === a.pos && b.site < a.site) ? { ...a, pos: a.pos + b.text.length } : a;
    // An insert strictly inside a concurrently deleted range is deleted with it on both sides (the delete grows on
    // the other replica), so here it becomes a no-op.
    if (a.type === "ins" && b.type === "del")
        return a.pos <= b.pos ? a : a.pos >= b.pos + b.n ? { ...a, pos: a.pos - b.n } : { ...a, pos: b.pos, text: "" };
    if (a.type === "del" && b.type === "ins")
        return b.pos <= a.pos
            ? { ...a, pos: a.pos + b.text.length }
            : b.pos >= a.pos + a.n
              ? a
              : { ...a, n: a.n + b.text.length };
    const aEnd = a.pos + a.n,
        bEnd = b.pos + b.n;
    if (bEnd <= a.pos) return { ...a, pos: a.pos - b.n };
    if (b.pos >= aEnd) return a;
    const overlap = Math.min(aEnd, bEnd) - Math.max(a.pos, b.pos);
    return { ...a, pos: Math.min(a.pos, b.pos), n: a.n - overlap };
}
const applyOp = (r, op) =>
    op.type === "ins" ? op.text && r.rawInsert(op.pos, op.text) : op.n > 0 && r.rawDelete(op.pos, op.n);
{
    let converged = 0;
    const ROUNDS = 400;
    const base = "collaborative editing converges when operations are transformed\n".repeat(20);
    const A = new Rope(base),
        B = new Rope(base);
    for (let round = 0; round < ROUNDS; round++) {
        const len = A.length;
        const mk = (site) => {
            const pos = rng.nextBounded(len);
            return rng.nextFloat() < 0.6
                ? { type: "ins", pos, text: `[${site}${round}]`, site }
                : { type: "del", pos, n: Math.min(1 + rng.nextBounded(8), len - pos), site };
        };
        const a = mk(1),
            b = mk(2);
        applyOp(A, a);
        applyOp(A, transform(b, a));
        applyOp(B, b);
        applyOp(B, transform(a, b));
        if (A.text() === B.text()) converged++;
    }
    results["both replicas converge after every concurrent pair"] = converged === ROUNDS;
}

print(
    Table(
        [
            ["document", `${rope.length.format()} chars in ${rope.blocks.length} blocks`],
            [
                "edits",
                `${edits.format()} in ${editMs.toFixed(0)} ms (${((editMs * 1000) / edits).toFixed(1)} us/edit incl. mirror)`,
            ],
            ["index rebuilds (block splits)", String(rope.rebuilds)],
        ],
        { head: ["buffer", "value"] },
    ),
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
