// covers: dyna:docdb.Collection.explain
// explain plans and runs a query and reports queryPlanner { namespace "db.coll", parsedQuery, winningPlan,
// rejectedPlans } and executionStats { nReturned, totalKeysExamined, totalDocsExamined, executionTimeMicros }.
// Oracles: nReturned equals the model's match count after skip/limit (tests/lib/docdb_model.js predicates) and
// equals find(...).length; a scan examines every document and no key; an _id equality is IDHACK (EOF when absent);
// an equality on an indexed field is FETCH over IXSCAN naming the index, examining exactly the matching documents
// and one key per distinct value (keys examined counts posting lists, not entries as MongoDB does); a sort no index
// serves is SORT over the input stage, one an index serves is not; an indexed sort over a scan is FETCH
// over a full-scan IXSCAN with its direction; an $or over two indexes is FETCH over OR. explain on a collection that
// does not exist created it (listCollections grew); it now answers an empty plan and leaves the database as it was.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { rng, genDocs, filters, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.explain");
const stages = (p) => { const out = []; for (let s = p; s; s = s.inputStage || (s.inputStages && s.inputStages[0])) out.push(s.stage); return out; };

t.test("nReturned and docs examined against the JS model", (a) => {
    const r = rng(0xe1), db = new Database("ex"), c = db.collection("c");
    const docs = genDocs(r, 300);
    c.insertMany(docs.map(clone));
    for (let i = 0; i < 40; i++) {
        for (const [f, pred] of filters(r)) {
            const skip = Math.floor(r() * 5), limit = Math.floor(r() * 30);
            const n = docs.filter(pred).length, want = Math.max(0, limit ? Math.min(n - skip, limit) : n - skip);
            const ex = c.explain(f, { skip, limit });
            if (!a.eq(ex.executionStats.nReturned, want, "nReturned " + J([f, skip, limit]))) return;
            a.eq(ex.executionStats.nReturned, c.find(f, { skip, limit }).length);
            if (Object.keys(f).length === 0 || f._id) continue;
            a.eq(ex.queryPlanner.winningPlan.stage, "COLLSCAN", "no index: a scan");
            a.eq(ex.executionStats.totalKeysExamined, 0);
        }
    }
    const ex = c.explain({ a: 3 });
    a.eq(ex.executionStats.totalDocsExamined, 300, "a scan reads every document");
    a.eq(ex.queryPlanner.namespace, "ex.c");
    a.eq(J(ex.queryPlanner.parsedQuery), J({ a: 3 }));
    a.deep(ex.queryPlanner.rejectedPlans, []);
    a.eq(typeof ex.executionStats.executionTimeMicros, "number");
    a.ok(ex.executionStats.executionTimeMicros >= 0);
});

t.test("plan shapes: IDHACK, EOF, IXSCAN, SORT, index sort, OR", (a) => {
    const r = rng(0xe2), db = new Database("ex2"), c = db.collection("c");
    const docs = genDocs(r, 400);
    c.insertMany(docs.map(clone));
    c.createIndex({ a: 1 }); c.createIndex({ b: 1 });
    const id = c.explain({ _id: 5 });
    a.eq(id.queryPlanner.winningPlan.stage, "IDHACK");
    a.eq(id.executionStats.nReturned, 1);
    const eof = c.explain({ _id: 99999 });
    a.eq(eof.queryPlanner.winningPlan.stage, "EOF", "an absent _id");
    a.eq(eof.executionStats.nReturned, 0);
    for (let k = 0; k < 10; k++) {
        const ex = c.explain({ a: k }), n = docs.filter((d) => d.a === k).length;
        a.deep(stages(ex.queryPlanner.winningPlan), ["FETCH", "IXSCAN"], "a = " + k);
        a.eq(ex.queryPlanner.winningPlan.inputStage.indexName, "a_1");
        a.eq(J(ex.queryPlanner.winningPlan.inputStage.keyPattern), J({ a: 1 }));
        a.eq(ex.executionStats.nReturned, n);
        a.eq(ex.executionStats.totalDocsExamined, n, "an exact index scan reads only the matches");
        a.eq(ex.executionStats.totalKeysExamined, n ? 1 : 0, "keys examined counts distinct keys (posting lists), not entries");
    }
    const rg = c.explain({ a: { $gte: 3, $lt: 7 } }).executionStats;
    a.eq(rg.totalKeysExamined, new Set(docs.filter((d) => d.a >= 3 && d.a < 7).map((d) => d.a)).size, "one per distinct key in range");
    a.eq(rg.nReturned, docs.filter((d) => d.a >= 3 && d.a < 7).length);
    a.deep(stages(c.explain({ n: { $gt: 10 } }, { sort: { n: 1 } }).queryPlanner.winningPlan), ["SORT", "COLLSCAN"], "a sort no index serves");
    const byA = c.explain({}, { sort: { a: -1 }, limit: 5 }).queryPlanner.winningPlan;
    a.deep(stages(byA), ["FETCH", "IXSCAN"], "an index serves the sort");
    a.eq(byA.inputStage.direction, "backward");
    a.eq(byA.inputStage.fullScanForSort, true);
    const or = c.explain({ $or: [{ a: 1 }, { b: "s2" }] });
    a.deep(stages(or.queryPlanner.winningPlan), ["FETCH", "OR", "IXSCAN"]);
    a.eq(or.queryPlanner.winningPlan.inputStage.inputStages.length, 2);
    a.eq(or.executionStats.nReturned, docs.filter((d) => d.a === 1 || d.b === "s2").length);
});

t.test("a missing collection answers an empty run and is not created", (a) => {
    const db = new Database("ex3");
    const ex = db.collection("none").explain({ a: 1 });
    a.eq(ex.queryPlanner.namespace, "ex3.none");
    a.eq(ex.executionStats.nReturned, 0);
    a.eq(ex.executionStats.totalDocsExamined, 0);
    a.deep(db.listCollections(), [], "explain created nothing");
});

await t.run();
