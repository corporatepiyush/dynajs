// covers: dyna:structures.Table
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_table_cov_ml_dataframe_mathx");
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
    

await __kitS.section("table size", async () => {
{
        const tbl=new Table();
        tbl.put("r1","c1",42); tbl.put("r1","c2",43); tbl.put("r2","c1",44);
        assertEq(tbl.size,3,"table size");
        assertEq(tbl.get("r1","c1"),42,"table get");
        assert(tbl.has("r1","c1"),"table has");
        assert(tbl.row("r1").length===2,"table row");
        assert(tbl.column("c1").length===2,"table column");
        assert(tbl.cells().length===3,"table cells");
        const bytes=tbl.serialize();
        const back=Table.deserialize(bytes);
        assertEq(back.get("r1","c1"),42,"table deserialize");
        const fw=new Fenwick(8);
        fw.update(0,5); fw.update(3,2);
        assertEq(fw.prefixSum(0),5,"fenwick prefix 0");
        assertEq(fw.prefixSum(3),7,"fenwick prefix 3");
        assertEq(fw.rangeQuery(1,3),2,"fenwick range");
        const st=new SegTree(8);
        st.update(0,5); st.update(3,2);
        assertEq(st.rangeQuery(0,7),7,"segtree sum");
        const stMin=new SegTree(6,"min");
        stMin.update(0,5); stMin.update(1,2);
        assertEq(stMin.rangeQuery(0,1),2,"segtree min");
        const bf=new BloomFilter(1024,5);
        for(let i=0;i<100;i++) bf.add("k"+i);
        assert(bf.mayContain("k0"),"bloom contains");
        const bs=new BitSet(10000);
        bs.set(9999);
        assert(bs.get(9999),"bitset large");
        const goodBytes=tbl.serialize();
        const bad=goodBytes.slice(); bad[5]^=0xff;
        await assertThrows(()=>Table.deserialize(bad),"table forge throws");
        const bfBytes=bf.serialize();
        const bfBack=BloomFilter.deserialize(bfBytes);
        assert(bfBack.mayContain("k0"),"bloom deserialize");
        const lru=new LRU(2);
        lru.put("a",1); lru.put("b",2); lru.put("c",3);
        assert(!lru.has("a"),"lru evict");
        assert(lru.has("c"),"lru has c");
        const dq=new Deque();
        dq.pushBack(1); dq.pushFront(0);
        assertEq(dq.length,2,"deque length");
        const rb=new RingBuffer(3);
        rb.push(1); rb.push(2); rb.push(3); rb.push(4);
        assert(rb.toArray().length===3,"ringbuffer overwrite");
        const uf=new UnionFind(5);
        uf.union(0,1);
        assert(uf.connected(0,1),"unionfind connected");
        assertEq(uf.count,4,"unionfind count");
    }
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
