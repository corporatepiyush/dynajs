// covers: dyna:structures.SortedMap
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_map_cov_ml_dataframe_mathx");
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
    

__kitS.section("sortedmap size", () => {
{
        const sm=new SortedMap();
        sm.set(10,"a"); sm.set(20,"b"); sm.set(5,"c");
        assertEq(sm.size,3,"sortedmap size");
        assertEq(sm.get(10),"a","sortedmap get");
        assertEq(sm.firstKey(),5,"sortedmap firstKey");
        assertEq(sm.lastKey(),20,"sortedmap lastKey");
        assertEq(sm.floorKey(15),10,"sortedmap floor");
        assertEq(sm.ceilKey(15),20,"sortedmap ceil");
        assert(sm.rangeQuery(5,15).length===2,"sortedmap range");
        const bytes=sm.serialize();
        const back=SortedMap.deserialize(bytes);
        assert(back.get(10)==="a","sortedmap deserialize");
        const sm2=new SortedMap();
        sm2.set(1,"a"); sm2.set(1,"b");
        assertEq(sm2.size,1,"sortedmap dup size");
        const ss=new SortedSet();
        ss.add(5); ss.add(1); ss.add(9);
        assertEq(ss.size,3,"sortedset size");
        assertEq(ss.first(),1,"sortedset first");
        assertEq(ss.last(),9,"sortedset last");
        assertEq(ss.floor(6),5,"sortedset floor");
        assertEq(ss.ceil(6),9,"sortedset ceil");
        assert(ss.rangeQuery(1,5).length===2,"sortedset range");
        const bytes2=ss.serialize();
        const back2=SortedSet.deserialize(bytes2);
        assertEq(back2.size,3,"sortedset deserialize");
        const ssLarge=new SortedSet();
        for(let i=0;i<1000;i++) ssLarge.add(i);
        assertEq(ssLarge.size,1000,"sortedset large 1000");
        const smLarge=new SortedMap();
        for(let i=0;i<1000;i++) smLarge.set(i,i*2);
        assertEq(smLarge.size,1000,"sortedmap large 1000");
    }
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
