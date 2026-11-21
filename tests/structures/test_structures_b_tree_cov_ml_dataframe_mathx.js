// covers: dyna:structures.BTree
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_b_tree_cov_ml_dataframe_mathx");
import { Heap, Trie, BTree, HyperLogLog, BitSet, SortedMap, SortedSet, Table, BloomFilter, LRU, Deque, Fenwick, SegTree, UnionFind, RingBuffer } from "dyna:structures";
import { Graph } from "dyna:graph";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== structures ===");
    

__kitS.section("btree size", () => {
{
        const b=new BTree();
        b.set(10,"a"); b.set(20,"b"); b.set(5,"c");
        assertEq(b.size,3,"btree size");
        assertEq(b.get(10),"a","btree get");
        assertEq(b.firstKey(),5,"btree firstKey");
        assertEq(b.lastKey(),20,"btree lastKey");
        assertEq(b.floorKey(15),10,"btree floor");
        assertEq(b.ceilKey(15),20,"btree ceil");
        assert(b.rangeQuery(5,15).length===2,"btree range");
        assert(b.delete(10)===true,"btree delete");
        assert(!b.has(10),"btree not has after delete");
        const bytes=b.serialize();
        const back=BTree.deserialize(bytes);
        assert(back.has(5),"btree deserialize");
        const b2=new BTree();
        b2.set(1,"a"); b2.set(1,"b");
        assertEq(b2.size,1,"btree duplicate size 1");
        assertEq(b2.get(1),"b","btree duplicate overwritten");
    }
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
