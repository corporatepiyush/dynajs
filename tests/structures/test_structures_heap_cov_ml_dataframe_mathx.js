// covers: dyna:structures.Heap
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_heap_cov_ml_dataframe_mathx");
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
    

await __kitS.section("heap empty pop undefined", async () => {
{
        const h=new Heap((a,b)=>a-b);
        assertEq(h.pop(),undefined,"heap empty pop undefined");
        assertEq(h.peek(),undefined,"heap empty peek undefined");
        assertEq(h.size,0,"heap size 0");
        for(let i=0;i<100;i++) h.push(i);
        assertEq(h.size,100,"heap size 100");
        assertEq(h.peek(),0,"heap peek min 0");
        let prev=-1;
        for(let i=0;i<100;i++){ const v=h.pop(); assert(v>=prev,"heap invariant "+i); prev=v; }
        assertEq(h.size,0,"heap drained");
        const h2=new Heap((a,b)=>a-b);
        h2.push(5); h2.push(5); h2.push(5);
        assertEq(h2.size,3,"heap dup size 3");
        assertEq(h2.pop(),5,"heap dup pop");
        const h3=new Heap((a,b)=>a-b);
        for(let i=0;i<5000;i++) h3.push(Math.floor(Math.random()*10000));
        assertEq(h3.size,5000,"heap large cap 5000");
        let last=h3.pop();
        for(let i=1;i<5000;i++){ const cur=h3.pop(); assert(cur>=last,"heap large invariant"); last=cur; }
        const hn=new Heap();
        hn.push(3); hn.push(1); hn.push(2);
        assertEq(hn.pop(),1,"natural heap min");
        await assertThrows(()=>new Heap(42),"Heap non-function throws");
        await assertThrows(()=>{ const hh=new Heap(); hh.push(1); hh.push("x"); },"natural heap refuses non-number");
        let shouldThrow=false;
        const h4=new Heap((a,b)=>{ if(shouldThrow) throw new Error("boom"); return a-b; });
        h4.push(5); h4.push(1); h4.push(3);
        assertEq(h4.size,3,"heap before throw size 3");
        shouldThrow=true;
        await assertThrows(()=>h4.push(2),"heap throwing comparator throws");
        shouldThrow=false;
        assertEq(h4.size,3,"heap size unchanged after throwing comparator rollback");
        h4.push(6);
        const out=[]; while(h4.size) out.push(h4.pop());
        assert(out.length===4,"heap after rollback 4 elements");
        let h5;
        h5=new Heap(async (a,b)=>{ await assertThrows(()=>h5.push(999),"reentrant push throws"); return a-b; });
        h5.push(3); h5.push(1); h5.push(2);
        assertEq(h5.size,3,"reentrant guard size");
        const hEmpty=new Heap((a,b)=>a-b);
        assertEq(hEmpty.pop(),undefined,"empty heap pop undefined again");
        assertEq(hEmpty.peek(),undefined,"empty heap peek undefined again");
    }
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
