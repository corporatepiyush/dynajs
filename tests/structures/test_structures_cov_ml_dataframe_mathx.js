// flags: --std
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import * as structures from "dyna:structures";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); throw new Error("assertion failed: " + msg); } }
function assertEq(a,b,msg){ n++; if(!Object.is(a,b)){ fails++; print("FAIL eq: "+msg+" got "+a+" want "+b); throw new Error("eq failed "+msg); } }
function assertClose(a,b,tol,msg){ n++; if(!(Math.abs(a-b)<=tol)){ fails++; print("FAIL close: "+msg+" "+a+" vs "+b); throw new Error("close failed "+msg); } }
async function assertThrows(fn, msg){ n++; let t=false; try{await fn();}catch(e){t=true;} if(!t){fails++; print("FAIL throws: "+msg); throw new Error("throws failed "+msg);} }
function assertThrowsType(fn, kind, msg){ n++; try{fn();}catch(e){ if(e instanceof kind) return; fails++; print("FAIL throwsType "+msg+" got "+e); throw new Error("throwsType failed "+msg); } fails++; print("FAIL throwsType not thrown "+msg); throw new Error("throwsType not thrown "+msg); }
function lcg(seed){ let s=seed>>>0; return ()=> (s=(s*1664525+1013904223)>>>0)/4294967296; }
print("=== ml ===");
print("=== structures ===");
{
    const {Heap, Graph, Trie, BTree, HyperLogLog, BitSet, SortedMap, SortedSet, Table, BloomFilter, LRU, Deque, Fenwick, SegTree, UnionFind, RingBuffer} = structures;
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
    {
        const g=new Graph({directed:false, weighted:true});
        for(let i=0;i<10;i++) g.addNode();
        for(let i=0;i<9;i++) g.addEdge(i,i+1,1);
        assertEq(g.nodeCount,10,"graph nodeCount");
        assertEq(g.edgeCount,9,"graph edgeCount");
        const d=g.dijkstra(0);
        assert(d[0]===0 && d[9]===9,"graph dijkstra");
        assert(g.hasEdge(0,1),"graph hasEdge");
        assert(!g.hasEdge(0,9),"graph not hasEdge");
        assert(g.neighbors(0).length===1,"graph neighbors");
        assert(g.bfs(0).length===10,"graph bfs");
        assert(g.dfs(0).length===10,"graph dfs");
        const fw=g.floydWarshall();
        assert(fw.length===10,"graph floyd");
        await assertThrows(()=>{ const gg=new Graph(); for(let i=0;i<1025;i++) gg.addNode(); gg.floydWarshall(); },"graph floyd >1024 throws");
    }
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
    {
        const b=new BitSet();
        b.set(3); b.set(65); b.set(130);
        assert(b.get(3) && b.get(65) && b.get(130),"bitset get");
        assert(b.count===3,"bitset count");
        b.clear(65);
        assert(!b.get(65) && b.count===2,"bitset clear");
        b.flip(3);
        assert(!b.get(3),"bitset flip clear");
        b.flip(7);
        assert(b.get(7),"bitset flip set");
        assertEq(b.nextSet(0),7,"bitset nextSet");
        const b2=new BitSet();
        b2.set(1); b2.set(7);
        b.and(b2);
        assert(b.get(7) && !b.get(130),"bitset and");
        const bytes=b.serialize();
        const back=BitSet.deserialize(bytes);
        assert(back.get(7),"bitset deserialize");
    }
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
}
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
