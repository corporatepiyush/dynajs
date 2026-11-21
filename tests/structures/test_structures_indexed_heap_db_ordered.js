// covers: dyna:structures.IndexedHeap
// Differential tests for the ordered containers added for interpreter-bound
// workloads: ZSet, OrderedMap, IndexedHeap, Interner. Each runs long random
// operation sequences against a brute-force JS model (sorted arrays, Maps) with
// several seeds, and checks every query the class answers after every batch.
// Keys include non-ASCII and astral characters: the order is UTF-8 byte order,
// which the model computes from TextEncoder bytes, not from JS `<`.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_indexed_heap_db_ordered");
import { ZSet, OrderedMap, IndexedHeap, Interner } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
function same(a, b, m) { const x = String(JSON.stringify(a)), y = String(JSON.stringify(b)); check(x === y, m + "\n   got  " + x.slice(0, 300) + "\n   want " + y.slice(0, 300)); }

function rng(seed) {
    let s = BigInt(seed) & 0xFFFFFFFFFFFFFFFFn;
    return () => {
        s = (s + 0x9E3779B97F4A7C15n) & 0xFFFFFFFFFFFFFFFFn;
        let z = s;
        z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & 0xFFFFFFFFFFFFFFFFn;
        z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & 0xFFFFFFFFFFFFFFFFn;
        return Number((z ^ (z >> 31n)) >> 11n) / 2 ** 53;
    };
}
const enc = new TextEncoder();
function bcmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b);
    const m = Math.min(x.length, y.length);
    for (let i = 0; i < m; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}
const ALPH = ["a", "b", "c", "ab", "é", "z", "\u{1F600}", "", "A", "0", ""];
function key(r, spread) { let s = ""; const len = 1 + Math.floor(r() * 3); for (let i = 0; i < len; i++) s += ALPH[Math.floor(r() * ALPH.length)]; return s + (spread ? Math.floor(r() * spread) : ""); }

// ---- ZSet -------------------------------------------------------------------
function zsetModel() {
    const m = new Map();
    const sorted = () => [...m].sort((p, q) => p[1] - q[1] || bcmp(p[0], q[0]));
    return { m, sorted };
}
function normRange(start, stop, len) {
    if (start < 0) start += len; if (stop < 0) stop += len; if (start < 0) start = 0;
    if (start > stop || start >= len) return null;
    if (stop >= len) stop = len - 1;
    return [start, stop];
}

for (const [seed, max] of [[11, false], [12, true], [13, false]]) {
    const r = rng(seed), h = new IndexedHeap({ max }), M = new Map();
    const top = () => { let best; for (const [id, p] of M) if (best === undefined || (max ? p > best[1] : p < best[1]) || (p === best[1] && id < best[0])) best = [id, p]; return best; };
    for (let step = 0; step < 6000; step++) {
        const op = r(), id = Math.floor(r() * 300) * (r() < 0.05 ? 14316557 : 1);
        if (op < 0.45) { const p = Math.floor(r() * 40) - 10 + (r() < 0.1 ? 0.5 : 0); h.push(id, p); M.set(id, p); }
        else if (op < 0.6) eq(h.delete(id), M.delete(id), `iheap[${seed}] delete`);
        else if (op < 0.75) { const t = top(); eq(h.pop(), t?.[0], `iheap[${seed}] pop @${step}`); if (t) M.delete(t[0]); }
        else if (op < 0.78) {
            const lim = Math.floor(r() * 20) - 5;
            const want = [...M].filter(([, p]) => max ? p >= lim : p <= lim).sort((a, b) => (max ? b[1] - a[1] : a[1] - b[1]) || a[0] - b[0]).map(e => e[0]);
            same(Array.from(h.popUntil(lim)), want, `iheap[${seed}] popUntil(${lim})`);
            for (const x of want) M.delete(x);
        }
        if (step % 50 === 0) {
            const t = top();
            eq(h.size, M.size, `iheap[${seed}] size`);
            eq(h.peek(), t?.[0], `iheap[${seed}] peek`);
            eq(h.peekPriority(), t?.[1], `iheap[${seed}] peekPriority`);
            eq(h.priority(id), M.get(id), `iheap[${seed}] priority`);
            eq(h.has(id), M.has(id), `iheap[${seed}] has`);
        }
        if (step % 1000 === 999) {
            const back = IndexedHeap.deserialize(h.serialize()), order = [], want = [];
            while (back.size) order.push(back.popEntry());
            const copy = new Map(M);
            while (copy.size) { let best; for (const [i2, p] of copy) if (best === undefined || (max ? p > best[1] : p < best[1]) || (p === best[1] && i2 < best[0])) best = [i2, p]; want.push(best); copy.delete(best[0]); }
            same(order, want, `iheap[${seed}] serialize + drain order`);
        }
    }
}

__kitS.section("iheap: pushMany then drain", () => {
    const h = new IndexedHeap();
    h.pushMany(new Uint32Array([5, 6, 7]), new Float64Array([3, 1, 2]));
    same([h.pop(), h.pop(), h.pop(), h.pop()], [6, 7, 5, undefined], "iheap: pushMany then drain");
    let threw = false; try { h.pushMany([1, 2], [1]); } catch (e) { threw = e instanceof RangeError; } check(threw, "iheap: pushMany length mismatch");
    threw = false; try { h.push(-1, 0); } catch (e) { threw = e instanceof RangeError; } check(threw, "iheap: negative id refused");
    threw = false; try { h.push(1, NaN); } catch (e) { threw = e instanceof TypeError; } check(threw, "iheap: NaN priority refused");
    h.push(4294967295, 1); eq(h.pop(), 4294967295, "iheap: max uint32 id");
    // Dijkstra on a grid with decrease-key, against a Bellman-Ford reference.
    const W = 30, Hh = 30, r = rng(99), w = new Float64Array(W * Hh).map(() => 1 + Math.floor(r() * 9));
    const dist = new Float64Array(W * Hh).fill(Infinity), q = new IndexedHeap();
    dist[0] = 0; q.push(0, 0);
    while (q.size) {
        const u = q.pop(), x = u % W, y = (u / W) | 0;
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
            const nx = x + dx, ny = y + dy; if (nx < 0 || ny < 0 || nx >= W || ny >= Hh) continue;
            const v = ny * W + nx, nd = dist[u] + w[v];
            if (nd < dist[v]) { dist[v] = nd; q.push(v, nd); }
        }
    }
    const bf = new Float64Array(W * Hh).fill(Infinity); bf[0] = 0;
    for (let changed = true; changed;) { changed = false; for (let u = 0; u < W * Hh; u++) { if (bf[u] === Infinity) continue; const x = u % W, y = (u / W) | 0;
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) { const nx = x + dx, ny = y + dy; if (nx < 0 || ny < 0 || nx >= W || ny >= Hh) continue; const v = ny * W + nx; if (bf[u] + w[v] < bf[v]) { bf[v] = bf[u] + w[v]; changed = true; } } } }
    check(dist.every((d, i) => d === bf[i]), "iheap: Dijkstra with decrease-key equals Bellman-Ford on a 30x30 grid");
});
__kitS.end();

print(`test_structures_db_ordered: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
