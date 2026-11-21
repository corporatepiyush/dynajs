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
//   groups      nested: platform inside eng inside all-staff, and legal and exec contain each other (a cycle); a
//               user's subjects are the BFS closure, memoised per user
//   rules       attribute conditions (tenant match, clearance >= classification, business hours, MFA for deletes)
//               indexed by action in a Multimap; deny rules override allows
//   decisions   decide(subject, action, resource) -> allow/deny with the rule or tuple path that decided it
//   cache       decisions in an LRU keyed by the request plus a store version; writing a tuple bumps the version, so
//               a stale allow can never be served after a revoke
//   reverse     listReaders(doc, action): every subject holding an implying relation on the doc or an ancestor
//               folder, groups expanded down the nesting, then the attribute rules
//   oracle      a naive evaluator replays the tuple log and closes group membership by fixed-point iteration (no code
//               shared with the engine); 4000 random requests must agree, listObjects must equal filtering every
//               document through decide(), and listReaders must equal every user through the naive evaluator
//
// MODULES  dyna:structures (RoaringBitmap, Multimap, LRU, Multiset) · dyna:random · dyna:cli ·
//          Array extensions
//
// RUN      dynajs examples/security/policy-engine.js

import { RoaringBitmap, Multimap, LRU, Multiset } from "dyna:structures";
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
const usersById = new Map(users.map((u) => [u.id, u]));
const groups = ["eng", "sales", "legal", "exec"].map((g) => `group:${g}`);
const membership = new Multimap();
for (const u of users) for (const g of groups) if (rng.nextFloat() < 0.2) membership.put(g, u.id);
// Nested groups (Zanzibar usersets on groups): every member of the child is a member of the parent. platform sits
// inside eng, eng inside all-staff, and legal and exec contain each other — a cycle the closure must survive.
groups.push("group:platform", "group:all-staff");
for (const u of users.slice(250, 270)) membership.put("group:platform", u.id);
const nested = new Multimap(); // child -> parents
for (const [child, parent] of [
    ["group:platform", "group:eng"],
    ["group:eng", "group:all-staff"],
    ["group:sales", "group:all-staff"],
    ["group:legal", "group:exec"],
    ["group:exec", "group:legal"],
])
    nested.put(child, parent);
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
        // kind -> relation -> subject -> RoaringBitmap of ids; lookups are three Map gets, no key strings
        this.grants = { doc: new Map(), folder: new Map() };
        for (const kind of ["doc", "folder"])
            for (const rel of Object.keys(IMPLIES)) this.grants[kind].set(rel, new Map());
        this.version = 0;
        this.log = [];
    }
    bitmap(kind, rel, subject) {
        return this.grants[kind].get(rel).get(subject);
    }
    grant(kind, rel, subject, id) {
        this.grants[kind]
            .get(rel)
            .getOrInsertComputed(subject, () => new RoaringBitmap())
            .add(id);
        this.log.push([kind, rel, subject, id, true]);
        this.version++;
    }
    revoke(kind, rel, subject, id) {
        this.bitmap(kind, rel, subject)?.delete(id);
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

// A user's subjects: the user plus `g#member` for every group reachable through direct membership and the nested
// child -> parent edges (BFS with a visited set, so the legal <-> exec cycle terminates). Memoised per user: the
// directory does not change during a run (a change would clear subjectMemo).
const directGroups = new Multimap();
for (const g of groups) for (const u of membership.get(g)) directGroups.put(u, g);
const subjectMemo = new Map();
function subjectsOf(userId) {
    let out = subjectMemo.get(userId);
    if (out) return out;
    const seen = new Set(directGroups.get(userId)),
        queue = [...seen];
    while (queue.length)
        for (const parent of nested.get(queue.pop())) if (!seen.has(parent)) (seen.add(parent), queue.push(parent));
    out = [userId, ...[...seen].map((g) => `${g}#member`)];
    subjectMemo.set(userId, out);
    return out;
}
const folderChain = (fid) => {
    const out = [];
    for (let f = fid; f >= 0; f = folders[f].parent) out.push(f);
    return out;
};
const CHAIN = folders.map((f) => folderChain(f.id));
const RELS_FOR = { viewer: ["owner", "editor", "viewer"], editor: ["owner", "editor"], owner: ["owner"] };

// Relationship check via bitmaps: does any of the user's subjects hold a relation implying `need` on the doc or an
// ancestor folder?
function relation(userId, need, doc) {
    const chain = CHAIN[doc.folder];
    for (const s of subjectsOf(userId))
        for (const rel of RELS_FOR[need]) {
            if (store.bitmap("doc", rel, s)?.has(doc.id)) return `${s} is ${rel} of doc:${doc.id}`;
            const fb = store.bitmap("folder", rel, s);
            if (fb) for (const f of chain) if (fb.has(f)) return `${s} is ${rel} of folder:${f}`;
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
// The live tuple set is the log replayed (last write wins), rebuilt only when the store version moves; group
// membership is a fixed point over (direct members, child -> parent) iterated until nothing changes — a different
// algorithm from the engine's BFS, sharing none of its code.
let naiveLive = null,
    naiveVersion = -1;
function naiveGroupsOf(userId) {
    const member = new Set(groups.filter((g) => membership.get(g).includes(userId)));
    for (let changed = true; changed;) {
        changed = false;
        for (const [child, parents] of nested.entries())
            for (const p of [parents].flat())
                if (member.has(child) && !member.has(p)) {
                    member.add(p);
                    changed = true;
                }
    }
    return member;
}
function naive(user, action, doc, env) {
    if (user.tenant !== doc.tenant) return false;
    if (["read", "comment", "write"].includes(action) && user.clearance < doc.classification) return false;
    if (["delete", "share"].includes(action) && !user.mfa) return false;
    if (["write", "delete"].includes(action) && env.freeze && doc.folder < 10) return false;
    if (naiveVersion !== store.version) {
        const live = new Map();
        for (const [kind, rel, subject, id, on] of store.log) live.set(`${kind}|${rel}|${subject}|${id}`, on);
        naiveLive = new Map(); // subject -> live tuples, so a check reads only its own subjects' rows
        for (const [k, on] of live) {
            if (!on) continue;
            const t = k.split("|");
            if (!naiveLive.has(t[2])) naiveLive.set(t[2], []);
            naiveLive.get(t[2]).push(t);
        }
        naiveVersion = store.version;
    }
    const need = ACTION_NEEDS[action],
        mine = new Set([user.id, ...[...naiveGroupsOf(user.id)].map((g) => `${g}#member`)]);
    const chain = new Set();
    for (let f = doc.folder; f >= 0; f = folders[f].parent) chain.add(f);
    for (const subject of mine)
        for (const [kind, rel, , id] of naiveLive.get(subject) ?? []) {
            if (!IMPLIES[rel].includes(need)) continue;
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
const childFolders = new Multimap();
for (const f of folders) if (f.parent >= 0) childFolders.put(String(f.parent), f.id);
const folderDocs = new Multimap();
for (const d of docs) folderDocs.put(String(d.folder), d.id);
function listViewable(user) {
    // union the user's doc bitmaps, then walk DOWN from every granted folder (each subtree once) collecting its docs
    const out = new RoaringBitmap(),
        folderSet = new RoaringBitmap();
    for (const s of subjectsOf(user.id))
        for (const rel of RELS_FOR.viewer) {
            const db = store.bitmap("doc", rel, s),
                fb = store.bitmap("folder", rel, s);
            if (db) out.or(db);
            if (fb) folderSet.or(fb);
        }
    const seen = new Set(),
        stack = [...folderSet];
    while (stack.length) {
        const f = stack.pop();
        if (seen.has(f)) continue;
        seen.add(f);
        for (const id of folderDocs.get(String(f))) out.add(id);
        stack.push(...childFolders.get(String(f)));
    }
    const allowed = new RoaringBitmap();
    for (const id of out) if (decide(user, "read", docs[id], {}).allow) allowed.add(id);
    return allowed;
}
// The reverse question — who can read this document? Subjects holding an implying relation on the doc or an
// ancestor folder, groups expanded DOWN the nesting (members of a group include members of its child groups),
// then the attribute rules through decide().
const nestedChildren = new Multimap(); // parent -> children
for (const [child, parent] of nested.entries()) nestedChildren.put(parent, child);
function membersOf(group) {
    const out = new Set(),
        seen = new Set([group]),
        queue = [group];
    while (queue.length) {
        const g = queue.pop();
        for (const u of membership.get(g)) out.add(u);
        for (const c of nestedChildren.get(g)) if (!seen.has(c)) (seen.add(c), queue.push(c));
    }
    return out;
}
function listReaders(doc, action = "read") {
    const candidates = new Set();
    const targets = [["doc", doc.id], ...CHAIN[doc.folder].map((f) => ["folder", f])];
    for (const rel of RELS_FOR[ACTION_NEEDS[action]])
        for (const [kind, id] of targets)
            for (const [subject, bm] of store.grants[kind].get(rel))
                if (bm.has(id)) {
                    if (subject.endsWith("#member")) for (const u of membersOf(subject.slice(0, -7))) candidates.add(u);
                    else candidates.add(subject);
                }
    return [...candidates].filter((id) => decide(usersById.get(id), action, doc, {}).allow).sort();
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

{
    // nested groups: a platform engineer in no other group reads through eng's and all-staff's folder grants
    const platformOnly = users.slice(250, 270).find((u) => directGroups.get(u.id).length === 1);
    results["nested membership reaches eng and all-staff"] =
        platformOnly !== undefined &&
        ["group:platform#member", "group:eng#member", "group:all-staff#member"].every((g) =>
            subjectsOf(platformOnly.id).includes(g),
        ) &&
        !subjectsOf(platformOnly.id).includes("group:sales#member");
    const legalOnly = users.find((u) => directGroups.get(u.id).join() === "group:legal");
    results["the legal <-> exec cycle closes without looping"] =
        legalOnly !== undefined &&
        subjectsOf(legalOnly.id)
            .filter((g) => g.startsWith("group:"))
            .sort()
            .join() === "group:exec#member,group:legal#member";
    // groups inherited only through nesting decide real checks: count decisions whose deciding subject is a group the
    // user is not a direct member of
    let viaNesting = 0;
    for (const u of users.slice(250, 270))
        for (const d of docs.slice(0, 600)) {
            const r = decide(u, "read", d, {});
            if (r.allow && /^group:/.test(r.by) && !directGroups.get(u.id).includes(r.by.split("#")[0])) viaNesting++;
        }
    results["some allows come only through a nested group"] = viaNesting > 0;
    // who-can-read against every user through the oracle
    let same = 0;
    const sizes = [];
    const t0 = performance.now();
    for (let i = 0; i < 60; i++) {
        const d = docs[i * 83],
            a = ["read", "write", "delete"][i % 3];
        const fast = listReaders(d, a),
            slow = users
                .filter((u) => naive(u, a, d, {}))
                .map((u) => u.id)
                .sort();
        if (fast.join() === slow.join()) same++;
        sizes.push(fast.length);
    }
    results["listReaders equals every user through the naive evaluator"] = same === 60 && sizes.some((n) => n > 0);
    print(
        `listReaders for 60 docs: ${(performance.now() - t0).toFixed(0)} ms incl. the oracle; median ${sizes.median()} readers, ${viaNesting} allows via nesting`,
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
