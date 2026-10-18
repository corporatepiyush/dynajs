// Differential tests for the ordered containers added for interpreter-bound
// workloads: ZSet, OrderedMap, IndexedHeap, Interner. Each runs long random
// operation sequences against a brute-force JS model (sorted arrays, Maps) with
// several seeds, and checks every query the class answers after every batch.
// Keys include non-ASCII and astral characters: the order is UTF-8 byte order,
// which the model computes from TextEncoder bytes, not from JS `<`.
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
for (const seed of [1, 2, 3, 77]) {
    const r = rng(seed), z = new ZSet(), M = zsetModel();
    const scores = [-1, 0, 0.5, 1, 2, 2, 3, 1e300, -Infinity, Infinity, -0];
    for (let step = 0; step < 4000; step++) {
        const op = r();
        const k = key(r, 40);
        if (op < 0.4) {
            const sc = r() < 0.5 ? scores[Math.floor(r() * scores.length)] : Math.floor(r() * 50) - 10;
            const isNew = !M.m.has(k);
            eq(z.add(k, sc), isNew, `zset[${seed}] add new flag`);
            M.m.set(k, sc);
        } else if (op < 0.55) {
            const d = Math.floor(r() * 7) - 3;
            const before = M.m.get(k) ?? 0;
            if (Number.isFinite(before)) { const nv = before + d; eq(z.incrBy(k, d), nv, `zset[${seed}] incrBy`); M.m.set(k, nv); }
        } else if (op < 0.7) {
            eq(z.delete(k), M.m.delete(k), `zset[${seed}] delete`);
        } else if (op < 0.74) {
            const lo = Math.floor(r() * 20) - 5, hi = lo + Math.floor(r() * 10);
            const want = M.sorted().filter(([, s]) => s >= lo && s <= hi);
            eq(z.removeRangeByScore(lo, hi), want.length, `zset[${seed}] removeRangeByScore count`);
            for (const [m] of want) M.m.delete(m);
        } else if (op < 0.77) {
            const s = M.sorted(), a = Math.floor(r() * 10) - 5, b = Math.floor(r() * 10) - 3;
            const nr = normRange(a, b, s.length), want = nr ? s.slice(nr[0], nr[1] + 1) : [];
            eq(z.removeRangeByRank(a, b), want.length, `zset[${seed}] removeRangeByRank(${a},${b})`);
            for (const [m] of want) M.m.delete(m);
        } else if (op < 0.79) {
            const s = M.sorted(), want = s[0];
            same(z.popMin(), want, `zset[${seed}] popMin`);
            if (want) M.m.delete(want[0]);
        } else if (op < 0.81) {
            const s = M.sorted(), want = s[s.length - 1];
            same(z.popMax(), want, `zset[${seed}] popMax`);
            if (want) M.m.delete(want[0]);
        }
        if (step % 97 === 0 || step === 3999) {
            const s = M.sorted();
            eq(z.size, s.length, `zset[${seed}] size @${step}`);
            same(z.entries(), s, `zset[${seed}] entries @${step}`);
            same([...z], s, `zset[${seed}] iterator @${step}`);
            for (let t = 0; t < 12; t++) {
                const kk = t < 6 && s.length ? s[Math.floor(r() * s.length)][0] : key(r, 40);
                const i = s.findIndex(([m]) => m === kk);
                eq(z.rank(kk), i < 0 ? undefined : i, `zset[${seed}] rank(${kk})`);
                eq(z.revRank(kk), i < 0 ? undefined : s.length - 1 - i, `zset[${seed}] revRank`);
                eq(z.score(kk), i < 0 ? undefined : s[i][1], `zset[${seed}] score`);
                eq(z.has(kk), i >= 0, `zset[${seed}] has`);
                const a = Math.floor(r() * 12) - 6, b = Math.floor(r() * 12) - 4;
                const nr = normRange(a, b, s.length);
                same(z.range(a, b), nr ? s.slice(nr[0], nr[1] + 1) : [], `zset[${seed}] range(${a},${b})`);
                same(z.range(a, b, { reverse: true }), nr ? [...s].reverse().slice(nr[0], nr[1] + 1) : [], `zset[${seed}] range rev(${a},${b})`);
                const at = Math.floor(r() * (s.length + 4)) - 2;
                same(z.at(at), at < 0 ? s[s.length + at] : s[at], `zset[${seed}] at(${at})`);
                const lo = Math.floor(r() * 30) - 12, hi = lo + Math.floor(r() * 20) - 2;
                const off = Math.floor(r() * 4), lim = Math.floor(r() * 6) + (r() < 0.3 ? 100 : 0);
                const inr = s.filter(([, x]) => x >= lo && x <= hi);
                eq(z.countByScore(lo, hi), inr.length, `zset[${seed}] countByScore(${lo},${hi})`);
                same(z.rangeByScore(lo, hi), inr, `zset[${seed}] rangeByScore(${lo},${hi})`);
                same(z.rangeByScore(lo, hi, { offset: off, limit: lim }), inr.slice(off, off + lim), `zset[${seed}] rangeByScore off/lim`);
                same(z.rangeByScore(lo, hi, { offset: off, limit: lim, reverse: true }), [...inr].reverse().slice(off, off + lim), `zset[${seed}] rangeByScore rev`);
            }
            same(ZSet.deserialize(z.serialize()).entries(), s, `zset[${seed}] serialize round trip @${step}`);
        }
    }
}
{
    const z = new ZSet();
    let threw = false; try { z.add("a", NaN); } catch (e) { threw = e instanceof TypeError; }
    check(threw, "zset: NaN score is a TypeError");
    z.add("a", Infinity);
    threw = false; try { z.incrBy("a", -Infinity); } catch (e) { threw = e instanceof RangeError; }
    check(threw, "zset: incrBy to NaN is a RangeError");
    eq(z.score("a"), Infinity, "zset: failed incrBy leaves the score");
    eq(z.popMin()?.[0], "a", "zset: popMin");
    eq(z.popMin(), undefined, "zset: popMin on empty");
    same(z.range(0, -1), [], "zset: empty range");
    eq(z.add(1, 2), true, "zset: a number member is its string");
    eq(z.has("1"), true, "zset: number member coerced");
}

// ---- OrderedMap -------------------------------------------------------------
for (const seed of [5, 6, 7]) {
    const r = rng(seed), o = new OrderedMap(), M = new Map();
    const sortedKeys = () => [...M.keys()].sort(bcmp);
    for (let step = 0; step < 4000; step++) {
        const op = r(), k = key(r, 30);
        if (op < 0.5) { const v = { v: step }; o.set(k, v); M.set(k, v); }
        else if (op < 0.7) eq(o.delete(k), M.delete(k), `omap[${seed}] delete`);
        else if (op < 0.73) {
            let lo = key(r, 30), hi = key(r, 30); if (bcmp(lo, hi) > 0) [lo, hi] = [hi, lo];
            const want = sortedKeys().filter(x => bcmp(x, lo) >= 0 && bcmp(x, hi) <= 0);
            eq(o.deleteRange(lo, hi), want.length, `omap[${seed}] deleteRange`);
            for (const x of want) M.delete(x);
        }
        if (step % 89 === 0 || step === 3999) {
            const ks = sortedKeys();
            eq(o.size, ks.length, `omap[${seed}] size`);
            same(o.keys(), ks, `omap[${seed}] keys @${step}`);
            check(o.entries().every(([kk, v], i) => kk === ks[i] && v === M.get(kk)), `omap[${seed}] entries identity @${step}`);
            eq(o.firstKey(), ks[0], `omap[${seed}] firstKey`);
            eq(o.lastKey(), ks[ks.length - 1], `omap[${seed}] lastKey`);
            for (let t = 0; t < 15; t++) {
                const q = t < 5 && ks.length ? ks[Math.floor(r() * ks.length)] : key(r, 30);
                eq(o.get(q), M.get(q), `omap[${seed}] get`);
                eq(o.has(q), M.has(q), `omap[${seed}] has`);
                eq(o.floorKey(q), ks.filter(x => bcmp(x, q) <= 0).pop(), `omap[${seed}] floorKey(${q})`);
                eq(o.ceilKey(q), ks.find(x => bcmp(x, q) >= 0), `omap[${seed}] ceilKey(${q})`);
                eq(o.lowerKey(q), ks.filter(x => bcmp(x, q) < 0).pop(), `omap[${seed}] lowerKey(${q})`);
                eq(o.higherKey(q), ks.find(x => bcmp(x, q) > 0), `omap[${seed}] higherKey(${q})`);
                eq(o.rank(q), ks.filter(x => bcmp(x, q) < 0).length, `omap[${seed}] rank(${q})`);
                const i = Math.floor(r() * (ks.length + 4)) - 2, wk = i < 0 ? ks[ks.length + i] : ks[i];
                eq(o.at(i)?.[0], wk, `omap[${seed}] at(${i})`);
                let lo = key(r, 30), hi = key(r, 30); if (bcmp(lo, hi) > 0) [lo, hi] = [hi, lo];
                const inr = ks.filter(x => bcmp(x, lo) >= 0 && bcmp(x, hi) <= 0), lim = Math.floor(r() * 5) + 1;
                same(o.rangeQuery(lo, hi).map(e => e[0]), inr, `omap[${seed}] rangeQuery`);
                same(o.rangeQuery(lo, hi, { limit: lim, reverse: true }).map(e => e[0]), [...inr].reverse().slice(0, lim), `omap[${seed}] rangeQuery rev limit`);
                const p = q.slice(0, 1 + Math.floor(r() * 2));
                const want = ks.filter(x => enc.encode(x).length >= enc.encode(p).length && bcmp(x.length ? x : "", p) >= 0 && enc.encode(x).slice(0, enc.encode(p).length).join() === enc.encode(p).join());
                same(o.prefix(p).map(e => e[0]), want, `omap[${seed}] prefix(${p})`);
            }
            const back = OrderedMap.deserialize(o.serialize());
            same(back.entries(), o.entries(), `omap[${seed}] serialize round trip`);
        }
    }
}
{
    const o = new OrderedMap();
    const cyc = { name: "x" }; cyc.self = o; o.set("k", cyc);
    eq(o.get("k").self, o, "omap: value holding the map itself");
    o.set("k", 2); eq(o.get("k"), 2, "omap: replace value");
    same(o.prefix(""), [["k", 2]], "omap: empty prefix is everything");
}

// ---- IndexedHeap ------------------------------------------------------------
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
{
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
}

// ---- Interner ---------------------------------------------------------------
{
    const r = rng(21), it = new Interner(), M = new Map(), rev = [];
    for (let step = 0; step < 20000; step++) {
        const s = r() < 0.01 ? "x".repeat(70000 + step) : key(r, 2000);
        if (!M.has(s)) { M.set(s, rev.length); rev.push(s); }
        eq(it.intern(s), M.get(s), "interner: intern id");
    }
    eq(it.size, rev.length, "interner: size");
    check(rev.every((s, i) => it.get(i) === s && it.lookup(s) === i && it.has(s)), "interner: get/lookup for every id");
    eq(it.lookup("never-seen-key"), undefined, "interner: lookup miss");
    eq(it.get(rev.length), undefined, "interner: get past the end");
    eq(it.get(-1), undefined, "interner: negative id");
    eq(it.get(1.5), undefined, "interner: fractional id");
    const batch = rev.slice(0, 500).concat(["new-a", "new-b", "new-a"]);
    const ids = it.internMany(batch);
    check(ids instanceof Uint32Array && ids.length === batch.length, "interner: internMany returns Uint32Array");
    check(batch.every((s, i) => it.get(ids[i]) === s), "interner: internMany ids resolve");
    eq(ids[batch.length - 1], ids[batch.length - 3], "interner: duplicate in a batch shares an id");
    same(it.getMany([0, 1, 1e9]), [rev[0], rev[1], undefined], "interner: getMany with unknown id");
    const back = Interner.deserialize(it.serialize());
    check(back.size === it.size && rev.every((s, i) => back.get(i) === s), "interner: serialize preserves ids");
    let threw = false; try { it.internMany(["a", 1]); } catch (e) { threw = e instanceof TypeError; } check(threw, "interner: non-string in internMany refused");
}

print(`test_structures_db_ordered: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
