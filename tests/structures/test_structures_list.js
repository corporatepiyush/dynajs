// flags: --std
// covers: dyna:structures.List
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_list");
import { BitSet, UnionFind, Deque, Fenwick, RingBuffer, SegTree, BloomFilter,
         Trie, LRU, SortedSet, SortedMap, Heap, List } from "dyna:structures";
import * as std from "std";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let threw = false;
    try { fn(); } catch { threw = true; }
    assert(threw, msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (!Object.is(a[i], b[i])) return false;
    return true;
}
function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

__kitS.section("new List is empty", () => {
    const l = new List();
        assert(l.length === 0, "new List is empty");
        assert(l.front() === undefined, "front on empty is undefined");
        assert(l.back() === undefined, "back on empty is undefined");
        assert(l.popFront() === undefined, "popFront on empty is undefined");
        assert(l.popBack() === undefined, "popBack on empty is undefined");

        assert(l.pushBack(2) === 1, "pushBack returns new length 1");
        assert(l.pushFront(1) === 2, "pushFront returns new length 2");
        assert(l.pushBack(3) === 3, "pushBack returns new length 3");
        assert(l.length === 3, "length is 3");
        assert(l.front() === 1, "front is 1");
        assert(l.back() === 3, "back is 3");
        assert(eqArr(l.toArray(), [1, 2, 3]), "toArray is [1,2,3]");

        assert(l.popFront() === 1, "popFront returns 1");
        assert(l.popBack() === 3, "popBack returns 3");
        assert(l.length === 1, "length is 1 after both pops");
        assert(l.front() === 2 && l.back() === 2, "single element is both front and back");
        assert(l.popFront() === 2, "final popFront returns 2");
        assert(l.length === 0, "empty after draining");
});

__kitS.section("interleaved push order", () => {
    const l = new List();
        l.pushBack(1);
        l.pushFront(0);
        l.pushBack(2);
        l.pushFront(-1);
        l.pushBack(3);
        assert(eqArr(l.toArray(), [-1, 0, 1, 2, 3]), "interleaved push order");
        assert(l.length === 5, "length matches after interleaving");
});

__kitS.section("length after bulk pushBack", () => {
    const l = new List();
        const N = 2000;
        for (let i = 0; i < N; i++) l.pushBack(i);
        assert(l.length === N, "length after bulk pushBack");
        assert(eqArr(l.toArray(), Array.from({ length: N }, (_, i) => i)),
               "bulk toArray matches ascending sequence");
        for (let i = 0; i < N; i++) assert(l.popFront() === i, "popFront drains ascending @" + i);
        assert(l.length === 0, "empty after draining forward");

        for (let i = 0; i < N; i++) l.pushFront(i);
        for (let i = 0; i < N; i++) assert(l.popFront() === N - 1 - i, "popFront drains reversed @" + i);
        assert(l.length === 0, "empty after draining reversed");
});

__kitS.section("object identity preserved in toArray", () => {
    const l = new List();
        const obj = { tag: "x" };
        l.pushBack(obj); l.pushBack("str"); l.pushBack(4.5);
        assert(l.toArray()[0] === obj, "object identity preserved in toArray");
        assert(l.popFront() === obj, "popFront returns the same object");
        assert(l.popFront() === "str", "popFront returns the string");
        assert(l.popFront() === 4.5, "popFront returns the number");
});

__kitS.section("for..of visits in list order", () => {
    const l = new List();
        l.pushBack(1); l.pushBack(2); l.pushBack(3);
        const seen = [];
        for (const v of l) seen.push(v);
        assert(eqArr(seen, [1, 2, 3]), "for..of visits in list order");
        assert(eqArr([...l], [1, 2, 3]), "spread operator works via [Symbol.iterator]");
        assert(l.length === 3, "length unaffected by iteration");

    const l2 = new List();
        const seen2 = [];
        for (const v of l2) seen2.push(v);
        assert(seen2.length === 0, "for..of over empty list yields nothing");
});

__kitS.section("List has no resource surface", () => {
    const l = new List();
    assert(l.close === undefined && l.closed === undefined &&
           l[Symbol.dispose] === undefined, "List has no resource surface");
    for (let i = 0; i < 500; i++) {
        const ll = new List();
        ll.pushBack({ i }); ll.pushFront("s" + i); ll.pushBack(i);
    }
    std.gc();
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
