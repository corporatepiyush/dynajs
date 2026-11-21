// covers: dyna:structures.Trie
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_trie_cov_ml_dataframe_mathx");
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
    

__kitS.section("trie has car", () => {
{
        const t=new Trie();
        t.insert("cat"); t.insert("car"); t.insert("card");
        assert(t.has("car"),"trie has car");
        assert(!t.has("ca"),"trie not has ca");
        assertEq(t.size,3,"trie size");
        assert(t.keysWithPrefix("car").length===2,"trie keysWithPrefix");
        assertEq(t.longestPrefix("cardance"),"card","trie longestPrefix");
        assert(t.delete("car")===true,"trie delete car");
        assert(!t.has("car") && t.has("card"),"trie delete keeps descendant");
        t.insert(""); assert(t.has(""),"trie empty string");
        const deep=new Trie();
        deep.insert("a".repeat(1000));
        assert(deep.has("a".repeat(1000)),"trie deep");
        const bytes=t.serialize();
        const back=Trie.deserialize(bytes);
        assert(back.has("cat"),"trie deserialize");
    }
});

__kitS.section("trie 5000 keys", () => {
{
        const t=new Trie();
        for(let i=0;i<5000;i++) t.insert("key"+i);
        assertEq(t.size,5000,"trie 5000 keys");
        const bt=new BTree();
        for(let i=0;i<2000;i++) bt.set(i,"v"+i);
        assertEq(bt.size,2000,"btree 2000");
        const bs=new BitSet();
        for(let i=0;i<1000;i++) bs.set(i*10);
        assertEq(bs.count,1000,"bitset 1000");
        const hll=new HyperLogLog(12);
        for(let i=0;i<5000;i++) hll.add("k"+i);
        assert(hll.count()>4000 && hll.count()<6000,"hll 5000 estimate");
    }
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
