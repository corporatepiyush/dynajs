// covers: dyna:structures.HyperLogLog
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_hyper_log_log_cov_ml_dataframe_mathx");
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
    

__kitS.section("hll count ~2", () => {
{
        const h=new HyperLogLog(10);
        h.add("a"); h.add("b"); h.add("a");
        assert(h.count()>=2 && h.count()<=3,"hll count ~2");
        const h2=new HyperLogLog(10);
        h2.add("c");
        h.merge(h2);
        assert(h.count()>=3,"hll merge");
        assertEq(h.precision,10,"hll precision");
        assert(h.registers>0,"hll registers");
        const bytes=h.serialize();
        const back=HyperLogLog.deserialize(bytes);
        assert(back.count()===h.count(),"hll deserialize count");
    }
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
