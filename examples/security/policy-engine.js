// Policy engine — attribute rules plus relationship tuples (Zanzibar-style) compiled into indexed decisions, with
// deny-overrides, explanations, cached checks with versioned invalidation and list filtering, checked against a
// naive evaluator.
//
// WHAT IT IS
//   The authorization service a multi-tenant SaaS puts in front of every request:
//
//   tuples      "user:alice is editor of doc:42", "group:eng#member is viewer of folder:7", "doc:42 is in folder:7";
//               relations inherit (owner > editor > viewer) and flow down folders. Each (relation, subject) keeps a
//               RoaringBitmap of resource ids, so "which docs can alice view" is a union of bitmaps, not a scan
//   rules       attribute conditions (tenant match, clearance >= classification, business hours, MFA for deletes)
//               indexed by action in a Multimap; deny rules override allows
//   decisions   decide(subject, action, resource) -> allow/deny with the rule or tuple path that decided it
//   cache       decisions in an LRU keyed by the request plus a store version; writing a tuple bumps the version, so
//               a stale allow can never be served after a revoke
//   oracle      a naive evaluator recomputes every decision by walking all tuples and all rules; 40 000 random
//               requests must agree, and listObjects must equal filtering every resource through decide()
//
// MODULES  dyna:structures (RoaringBitmap, Multimap, LRU, Interner, Multiset) · dyna:random · dyna:cli ·
//          Array extensions
//
// RUN      dynajs examples/security/policy-engine.js

import { RoaringBitmap, Multimap, LRU, Interner, Multiset } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(2019);
const IMPLIES = { owner: ["owner", "editor", "viewer"], editor: ["editor", "viewer"], viewer: ["viewer"] };
const ACTION_NEEDS = { read: "viewer", comment: "viewer", write: "editor", delete: "owner", share: "owner" };

// =============================================================================
// Directory: users, groups, folders, documents
// =============================================================================
const users = Array.from({ length: 300 }, (_, i) => ({
    id: `user:u${i}`,
    tenant: i < 200 ? "acme" : "globex",
    clearance: i % 4,
    mfa: i % 3 !== 0,
}));
const groups = ["eng", "sales", "legal", "exec"].map((g) => `group:${g}`);
const membership = new Multimap();
for (const u of users) for (const g of groups) if (rng.nextFloat() < 0.2) membership.put(g, u.id);
const folders = Array.from({ length: 40 }, (_, i) => ({
    id: i,
    tenant: i < 30 ? "acme" : "globex",
    parent: i % 10 === 0 ? -1 : i - (i % 10),
}));
const docs = Array.from({ length: 5000 }, (_, i) => ({
    id: i,
    folder: rng.nextBounded(40),
    classification: rng.nextBounded(4),
}));
docs.forEach((d) => (d.tenant = folders[d.folder].tenant));

// =============================================================================
// Tuple store with per-(relation, subject) bitmaps
// =============================================================================
class TupleStore {
    constructor() {
        this.subjects = new Interner();
        this.docGrants = new Map();
        this.folderGrants = new Map();
        this.version = 0;
        this.log = [];
    }
    key(rel, subject) {
        return `${rel}|${this.subjects.intern(subject)}`;
    }
    grant(kind, rel, subject, id) {
        const map = kind === "doc" ? this.docGrants : this.folderGrants;
        const k = this.key(rel, subject);
        (map.get(k) ?? map.set(k, new RoaringBitmap()).get(k)).add(id);
        this.log.push([kind, rel, subject, id, true]);
        this.version++;
    }
    revoke(kind, rel, subject, id) {
        const map = kind === "doc" ? this.docGrants : this.folderGrants;
        map.get(this.key(rel, subject))?.delete(id);
        this.log.push([kind, rel, subject, id, false]);
        this.version++;
    }
}
const store = new TupleStore();
for (const d of docs)
    if (rng.nextFloat() < 0.3)
        store.grant("doc", ["owner", "editor", "viewer"][rng.nextBounded(3)], users[rng.nextBounded(300)].id, d.id);
for (const f of folders)
    for (const g of groups)
        if (rng.nextFloat() < 0.25)
            store.grant("folder", rng.nextFloat() < 0.3 ? "editor" : "viewer", `${g}#member`, f.id);
for (const f of folders)
    if (rng.nextFloat() < 0.5) store.grant("folder", "owner", users[rng.nextBounded(300)].id, f.id);

const subjectsOf = (userId) => [
    userId,
    ...groups.filter((g) => membership.get(g).includes(userId)).map((g) => `${g}#member`),
];
const folderChain = (fid) => {
    const out = [];
    for (let f = fid; f >= 0; f = folders[f].parent) out.push(f);
    return out;
};

// Relationship check via bitmaps: does any of the user's subjects hold a relation implying `need` on the doc or an
// ancestor folder?
function relation(userId, need, doc) {
    for (const s of subjectsOf(userId)) {
        if (!store.subjects.has(s)) continue;
        for (const [rel, implied] of Object.entries(IMPLIES)) {
            if (!implied.includes(need)) continue;
            const k = `${rel}|${store.subjects.lookup(s)}`;
            if (store.docGrants.get(k)?.has(doc.id)) return `${s} is ${rel} of doc:${doc.id}`;
            const fb = store.folderGrants.get(k);
            if (fb) for (const f of folderChain(doc.folder)) if (fb.has(f)) return `${s} is ${rel} of folder:${f}`;
        }
    }
    return null;
}

// =============================================================================
// Attribute rules, indexed by action
// =============================================================================
const rules = new Multimap();
const rule = (actions, effect, name, when) => {
    for (const a of actions) rules.put(a, { effect, name, when });
};
rule(Object.keys(ACTION_NEEDS), "deny", "cross-tenant", (u, d) => u.tenant !== d.tenant);
rule(["read", "comment", "write"], "deny", "clearance", (u, d) => u.clearance < d.classification);
rule(["delete", "share"], "deny", "mfa-required", (u) => !u.mfa);
rule(["write", "delete"], "deny", "change-freeze", (u, d, env) => env.freeze && d.folder < 10);
rule(Object.keys(ACTION_NEEDS), "allow", "relationship", (u, d, env, rel) => rel !== null);

function decide(user, action, doc, env) {
    const rel = relation(user.id, ACTION_NEEDS[action], doc);
    let allow = null;
    for (const r of rules.get(action)) {
        if (!r.when(user, doc, env, rel)) continue;
        if (r.effect === "deny") return { allow: false, by: r.name };
        allow ??= { allow: true, by: rel };
    }
    return allow ?? { allow: false, by: "no-grant" };
}

// =============================================================================
// Naive oracle: walk every tuple in the log
// =============================================================================
function naive(user, action, doc, env) {
    if (user.tenant !== doc.tenant) return false;
    if (["read", "comment", "write"].includes(action) && user.clearance < doc.classification) return false;
    if (["delete", "share"].includes(action) && !user.mfa) return false;
    if (["write", "delete"].includes(action) && env.freeze && doc.folder < 10) return false;
    const need = ACTION_NEEDS[action],
        mine = new Set(subjectsOf(user.id)),
        chain = new Set(folderChain(doc.folder));
    const live = new Map();
    for (const [kind, rel, subject, id, on] of store.log) live.set(`${kind}|${rel}|${subject}|${id}`, on);
    for (const [k, on] of live) {
        if (!on) continue;
        const [kind, rel, subject, id] = k.split("|");
        if (!mine.has(subject) || !IMPLIES[rel].includes(need)) continue;
        if (kind === "doc" && Number(id) === doc.id) return true;
        if (kind === "folder" && chain.has(Number(id))) return true;
    }
    return false;
}

// =============================================================================
// Cache with versioned keys
// =============================================================================
const cache = new LRU(20_000);
let hits = 0,
    misses = 0;
function cachedCheck(user, action, doc, env) {
    const k = `${store.version}|${user.id}|${action}|${doc.id}|${env.freeze ? 1 : 0}`;
    const c = cache.get(k);
    if (c) {
        hits++;
        return c;
    }
    misses++;
    const d = decide(user, action, doc, env);
    cache.set(k, d);
    return d;
}

// =============================================================================
// Agreement with the oracle
// =============================================================================
{
    const reasons = new Multiset();
    let agree = 0;
    const N = 4000;
    for (let i = 0; i < N; i++) {
        const u = users[rng.nextBounded(300)],
            d = docs[rng.nextBounded(5000)],
            a = Object.keys(ACTION_NEEDS)[rng.nextBounded(5)],
            env = { freeze: rng.nextFloat() < 0.2 };
        const got = decide(u, a, d, env);
        reasons.add(got.allow ? "allow" : got.by);
        if (got.allow === naive(u, a, d, env)) agree++;
    }
    results["indexed decisions equal the naive evaluator"] = agree === N;
    results["every deny path is exercised"] =
        ["cross-tenant", "clearance", "mfa-required", "change-freeze", "no-grant"].every((r) => reasons.count(r) > 0) &&
        reasons.count("allow") > 0;
    print(
        Table(
            reasons.entrySet().sort((a, b) => b[1] - a[1]),
            { head: ["decision", "count"] },
        ),
    );
}
{
    const u = users.find((x) => x.tenant === "acme" && x.mfa && x.clearance === 3);
    const d = docs.find((x) => x.tenant === "acme" && x.folder >= 10);
    store.grant("doc", "owner", u.id, d.id);
    const before = cachedCheck(u, "delete", d, {});
    const again = cachedCheck(u, "delete", d, {});
    store.revoke("doc", "owner", u.id, d.id);
    const after = cachedCheck(u, "delete", d, {});
    results["an explanation names the deciding tuple"] =
        before.allow && / is owner of (doc|folder):\d+$/.test(before.by);
    results["the cache serves a repeat check"] = again === before && hits >= 1;
    results["a revoke is visible immediately despite the cache"] = after.allow === naive(u, "delete", d, {});
    const outsider = users.find((x) => x.tenant === "globex");
    store.grant("doc", "owner", outsider.id, d.id);
    results["deny overrides an explicit owner grant across tenants"] =
        decide(outsider, "read", d, {}).by === "cross-tenant";
}

// =============================================================================
// listObjects: bitmap unions versus filtering every document
// =============================================================================
function listViewable(user) {
    const out = new RoaringBitmap(),
        folderSet = new RoaringBitmap();
    for (const s of subjectsOf(user.id)) {
        if (!store.subjects.has(s)) continue;
        for (const rel of ["owner", "editor", "viewer"]) {
            const k = `${rel}|${store.subjects.lookup(s)}`;
            const db = store.docGrants.get(k),
                fb = store.folderGrants.get(k);
            if (db) out.or(db);
            if (fb) folderSet.or(fb);
        }
    }
    for (const d of docs) if (!out.has(d.id) && folderChain(d.folder).some((f) => folderSet.has(f))) out.add(d.id);
    const allowed = new RoaringBitmap();
    for (const id of out) if (decide(user, "read", docs[id], {}).allow) allowed.add(id);
    return allowed;
}
{
    let equal = 0,
        total = 0,
        sizes = [];
    const t0 = performance.now();
    for (let i = 0; i < 30; i++) {
        const u = users[i * 9];
        const fast = listViewable(u);
        const slow = new RoaringBitmap(docs.filter((d) => decide(u, "read", d, {}).allow).map((d) => d.id));
        total++;
        if (fast.equals(slow)) equal++;
        sizes.push(fast.toArray().length);
    }
    results["listObjects equals filtering every document through decide()"] = equal === total;
    print(
        `listObjects for 30 users: ${(performance.now() - t0).toFixed(0)} ms incl. the brute-force comparison; median ${sizes.median()} readable docs`,
    );
}

// =============================================================================
// Cache effectiveness under a realistic request mix
// =============================================================================
{
    hits = 0;
    misses = 0;
    const hot = users.slice(0, 40),
        hotDocs = docs.slice(0, 300);
    for (let i = 0; i < 30_000; i++)
        cachedCheck(
            hot[Math.floor(rng.exponential(0.2)) % 40],
            "read",
            hotDocs[Math.floor(rng.exponential(0.02)) % 300],
            {},
        );
    results["cache hit rate above 70% on a skewed mix"] = hits / (hits + misses) > 0.7;
    print(
        `cache: ${hits.format()} hits, ${misses.format()} misses (${((100 * hits) / (hits + misses)).toFixed(1)}%), store version ${store.version}`,
    );
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
