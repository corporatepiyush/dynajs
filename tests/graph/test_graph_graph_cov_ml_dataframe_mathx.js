// covers: dyna:graph.Graph
// Split from tests/test_cov_ml_dataframe_mathx.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_cov_ml_dataframe_mathx");
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
    

await __kitS.section("graph nodeCount", async () => {
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
if (fails===0) print("test_structures_cov_ml_dataframe_mathx: all " + n + " assertions passed");
else print("test_structures_cov_ml_dataframe_mathx: " + fails + " FAILED of " + n);
});
__kitS.end();
