// Access control service — Zanzibar-style relationship-based authorisation with schema rewrites, nested groups,
// folder inheritance, conditional grants, consistency tokens, explanations, a decision cache and policy tests.
//
// WHAT IT IS
//   The central "can user U do action A on object O?" service that a document product's every request calls:
//
//   schema       YAML namespaces with relations and rewrites: a document's viewer = its viewers ∪ its editors ∪
//                the viewers of its parent folder; editor = editors ∪ owner; a group's member = members ∪ members of
//                nested groups (computed and tuple-to-userset rewrites, as in the Zanzibar paper)
//   tuples       relationship tuples (document:roadmap#editor@group:eng#member) stored in SQLite with a
//                monotonically increasing revision per write
//   conditions   a tuple may carry a caveat — valid only within a time window or from a network prefix
//                (contractors from the VPN only, a share link until Friday)
//   consistency  each write returns a revision token; a check "at least as fresh as" that token is never answered
//                from a stale cache entry (the new-enemy problem)
//   evaluation   recursive check with cycle protection and a depth limit; expand() returns the explanation tree
//                so support staff can answer "why can Dave see this?"
//   lookups      list every object of a type a user can view (reverse index walk)
//   API          POST /write, POST /check, POST /expand, GET /lookup — callers authenticate with a service JWT
//   tests        policy assertions in YAML (allow/deny cases) run against the live service
//   audit        structured decision logs (JSON lines) with the revision used
//   Leopard      Zanzibar's nested-group index: after every write, each group's transitive membership is flattened
//                into a RoaringBitmap of interned user ids (fixpoint over child groups, cycles included), so a
//                "is U a member of group G" step inside a check is one bitmap probe instead of a recursive walk;
//                groups with caveated or wildcard members, and checks at an older revision, fall back to recursion,
//                and explanations always recurse so they can name the path
//
// MODULES  dyna:http · dyna:net (SQLite, Prefix) · dyna:yaml · dyna:crypto (JWT) · dyna:structures (LRU, Interner,
//          RoaringBitmap) · dyna:log (Logger) · dyna:time · dyna:cli (Table)
//
// RUN      dynajs examples/security/access-control.js     loads an organisation, runs requests and the policy suite

import { App } from "dyna:http";
import { SQLite, Prefix } from "dyna:net";
import * as YAML from "dyna:yaml";
import { JWTSign, JWTVerify, RandomBytes } from "dyna:crypto";
import { LRU, Interner, RoaringBitmap } from "dyna:structures";
import { Logger } from "dyna:log";
import { parseRFC3339 } from "dyna:time";
import { Table } from "dyna:cli";
import { makeTempDir, removeAll } from "dyna:file";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("authz");

// =============================================================================
// Schema
// =============================================================================
const SCHEMA = YAML.Parse(`
user: {}
group:
  member: { union: [this, { tupleToUserset: { tupleset: member, computed: member } }] }
folder:
  parent: { union: [this] }
  owner: { union: [this] }
  editor: { union: [this, { computed: owner }, { tupleToUserset: { tupleset: parent, computed: editor } }] }
  viewer: { union: [this, { computed: editor }, { tupleToUserset: { tupleset: parent, computed: viewer } }] }
document:
  parent: { union: [this] }
  owner: { union: [this] }
  editor: { union: [this, { computed: owner }, { tupleToUserset: { tupleset: parent, computed: editor } }] }
  commenter: { union: [this, { computed: editor }] }
  viewer: { union: [this, { computed: commenter }, { tupleToUserset: { tupleset: parent, computed: viewer } }] }
  banned: { union: [this] }
  can_view: { exclusion: { base: viewer, subtract: banned } }
`);
const PERMISSION = { read: "can_view", comment: "commenter", write: "editor", share: "owner" };

// =============================================================================
// Tuple store
// =============================================================================
const db = new SQLite(work.join("tuples.db"));
db.exec(`CREATE TABLE tuples (object TEXT, relation TEXT, subject TEXT, subject_rel TEXT, caveat TEXT, created_rev INTEGER, deleted_rev INTEGER,
    PRIMARY KEY (object, relation, subject, subject_rel, created_rev))`);
db.exec("CREATE INDEX by_subject ON tuples(subject, subject_rel)");
db.exec("CREATE TABLE meta (k TEXT PRIMARY KEY, v INTEGER)");
db.exec("INSERT INTO meta VALUES ('rev', 0)");
const revision = () => db.query("SELECT v FROM meta WHERE k = 'rev'")[0].v;
function parseTuple(t) {
    // "document:roadmap#editor@group:eng#member"
    const m = /^([a-z]+:[\w.-]+)#(\w+)@([a-z]+:[\w.*-]+)(?:#(\w+))?$/.exec(t);
    if (!m) throw new Error(`malformed tuple: ${t}`);
    const type = m[1].split(":")[0];
    if (!SCHEMA[type]?.[m[2]]) throw new Error(`${type} has no relation ${m[2]}`);
    return { object: m[1], relation: m[2], subject: m[3], subjectRel: m[4] || "" };
}
function write(updates) {
    db.exec("BEGIN");
    try {
        const rev = revision() + 1;
        for (const u of updates) {
            const t = parseTuple(u.tuple);
            if (u.op === "delete")
                db.exec(
                    "UPDATE tuples SET deleted_rev = ? WHERE object = ? AND relation = ? AND subject = ? AND subject_rel = ? AND deleted_rev IS NULL",
                    [rev, t.object, t.relation, t.subject, t.subjectRel],
                );
            else
                db.exec("INSERT INTO tuples VALUES (?,?,?,?,?,?,NULL)", [
                    t.object,
                    t.relation,
                    t.subject,
                    t.subjectRel,
                    u.caveat ? JSON.stringify(u.caveat) : null,
                    rev,
                ]);
        }
        db.exec("UPDATE meta SET v = ? WHERE k = 'rev'", [rev]);
        db.exec("COMMIT");
        buildLeopard(rev);
        return rev;
    } catch (e) {
        db.exec("ROLLBACK");
        throw e;
    }
}
const live = "created_rev <= ?2 AND (deleted_rev IS NULL OR deleted_rev > ?2)";
const direct = (object, relation, rev) =>
    db.query(`SELECT * FROM tuples WHERE object = ?1 AND relation = ?3 AND ${live}`, [object, rev, relation]);

// =============================================================================
// Leopard: flattened group membership
// =============================================================================
const people = new Interner();
let leopard = { rev: -1, members: new Map(), unsafe: new Set() };
function buildLeopard(rev) {
    const rows = db.query(
        "SELECT object, subject, subject_rel, caveat FROM tuples WHERE relation = 'member' AND object LIKE 'group:%' AND created_rev <= ?1 AND (deleted_rev IS NULL OR deleted_rev > ?1)",
        [rev],
    );
    const members = new Map(),
        children = new Map(),
        unsafe = new Set();
    for (const r of rows) {
        const own = members.getOrInsertComputed(r.object, () => new RoaringBitmap());
        if (r.caveat || r.subject === "user:*") unsafe.add(r.object);
        else if (r.subject_rel === "member" && r.subject.startsWith("group:"))
            children.getOrInsertComputed(r.object, () => []).push(r.subject);
        else if (!r.subject_rel) own.add(people.intern(r.subject));
        else unsafe.add(r.object);
    }
    for (let changed = true; changed;) {
        // fixpoint: a cycle simply stops growing
        changed = false;
        for (const [g, kids] of children) {
            const set = members.get(g),
                before = set.size;
            for (const k of kids) {
                if (unsafe.has(k)) unsafe.add(g);
                const m = members.get(k);
                if (m) set.or(m);
            }
            if (set.size !== before) changed = true;
        }
    }
    leopard = { rev, members, unsafe };
}
let leopardHits = 0;

// =============================================================================
// Evaluation
// =============================================================================
function caveatOk(caveat, ctx) {
    if (!caveat) return { ok: true };
    const c = JSON.parse(caveat);
    if (c.notAfter && ctx.now > parseRFC3339(c.notAfter).sec * 1000) return { ok: false, why: `expired ${c.notAfter}` };
    if (c.network && !(ctx.ip && new Prefix(c.network).contains(ctx.ip)))
        return { ok: false, why: `requires network ${c.network}` };
    return { ok: true, why: c.network ? `from ${c.network}` : c.notAfter ? `until ${c.notAfter}` : "" };
}
const MAX_DEPTH = 24;
function checkRelation(object, relation, user, ctx, depth = 0, seen = new Set()) {
    const key = `${object}#${relation}`;
    if (depth > MAX_DEPTH) throw new Error("check exceeded the maximum depth");
    if (seen.has(key)) return { allowed: false, node: { key, cycle: true } };
    seen = new Set(seen).add(key);
    const [type] = object.split(":");
    const rule = SCHEMA[type]?.[relation];
    if (!rule) return { allowed: false, node: { key, unknown: true } };
    if (rule.exclusion) {
        const base = checkRelation(object, rule.exclusion.base, user, ctx, depth + 1, seen),
            sub = checkRelation(object, rule.exclusion.subtract, user, ctx, depth + 1, seen);
        return { allowed: base.allowed && !sub.allowed, node: { key, exclusion: [base.node, sub.node] } };
    }
    const children = [];
    for (const part of rule.union) {
        let r;
        if (part === "this") r = checkThis(object, relation, user, ctx, depth, seen);
        else if (part.computed) r = checkRelation(object, part.computed, user, ctx, depth + 1, seen);
        else {
            const parents = direct(object, part.tupleToUserset.tupleset, ctx.rev);
            r = { allowed: false, node: { key: `${key}->${part.tupleToUserset.tupleset}`, children: [] } };
            for (const p of parents) {
                const sub = checkRelation(p.subject, part.tupleToUserset.computed, user, ctx, depth + 1, seen);
                r.node.children.push(sub.node);
                if (sub.allowed) {
                    r.allowed = true;
                    break;
                }
            }
        }
        children.push(r.node);
        if (r.allowed) return { allowed: true, node: { key, via: r.node } };
    }
    return { allowed: false, node: { key, children } };
}
function checkThis(object, relation, user, ctx, depth, seen) {
    const node = { key: `${object}#${relation} (direct)`, children: [] };
    for (const t of direct(object, relation, ctx.rev)) {
        const cv = caveatOk(t.caveat, ctx);
        if (!t.subject_rel && (t.subject === user || t.subject === "user:*")) {
            if (cv.ok)
                return {
                    allowed: true,
                    node: { key: `${t.subject} is ${relation} of ${object}${cv.why ? ` (${cv.why})` : ""}` },
                };
            node.children.push({ key: `${t.subject}: caveat failed, ${cv.why}` });
        } else if (t.subject_rel) {
            if (!cv.ok) {
                node.children.push({ key: `${t.subject}#${t.subject_rel}: caveat failed, ${cv.why}` });
                continue;
            }
            if (
                !ctx.explain &&
                t.subject_rel === "member" &&
                leopard.rev === ctx.rev &&
                leopard.members.has(t.subject) &&
                !leopard.unsafe.has(t.subject)
            ) {
                leopardHits++;
                if (leopard.members.get(t.subject).has(people.intern(user)))
                    return { allowed: true, node: { key: `${user} is in ${t.subject} (Leopard index)` } };
                continue;
            }
            const sub = checkRelation(t.subject, t.subject_rel, user, ctx, depth + 1, seen);
            node.children.push(sub.node);
            if (sub.allowed)
                return {
                    allowed: true,
                    node: { key: `${object}#${relation} via ${t.subject}#${t.subject_rel}`, via: sub.node },
                };
        }
    }
    return { allowed: false, node };
}
function lookup(type, permission, user, ctx) {
    const objects = db
        .query(`SELECT DISTINCT object FROM tuples WHERE object LIKE ?1 AND ${live}`, [`${type}:%`, ctx.rev])
        .map((r) => r.object);
    return objects.filter((o) => checkRelation(o, PERMISSION[permission] || permission, user, ctx).allowed).sort();
}
const render = (node, indent = "") =>
    [
        indent + node.key,
        ...(node.via ? [render(node.via, indent + "  ")] : []),
        ...(node.children || [])
            .filter((c) => c.via || c.children?.length || !c.key.endsWith("(direct)"))
            .map((c) => render(c, indent + "  ")),
        ...(node.exclusion || []).map((c) => render(c, indent + "  ")),
    ].join("\n");

// =============================================================================
// Service
// =============================================================================
const SERVICE_KEY = RandomBytes(32);
const cache = new LRU(20000);
const audit = [];
const log = new Logger({
    name: "authz",
    format: "json",
    timestamp: "iso",
    dest: (line) => audit.push(JSON.parse(line)),
});
const stats = { checks: 0, cacheHits: 0, denied: 0 };
const app = new App({ port: 0 });
app.use((req) => {
    try {
        req.caller = JWTVerify(String(req.headers.authorization || "").replace(/^Bearer /, ""), SERVICE_KEY, {
            algorithms: ["HS256"],
        });
    } catch {
        return {
            response: {
                status: 401,
                body: JSON.stringify({ error: "service token required" }),
                contentType: "application/json",
            },
        };
    }
    if (req.method === "POST" && req.path === "/write" && !req.caller.scopes?.includes("write"))
        return {
            response: {
                status: 403,
                body: JSON.stringify({ error: "write scope required" }),
                contentType: "application/json",
            },
        };
});
app.post("/write", (req) => {
    try {
        return { revision: write(JSON.parse(req.body).updates) };
    } catch (e) {
        return { status: 400, body: JSON.stringify({ error: e.message }), contentType: "application/json" };
    }
});
app.post("/check", (req) => {
    const { user, action, object, atLeast, ip, now } = JSON.parse(req.body);
    const rev = revision();
    if (atLeast && atLeast > rev)
        return {
            status: 409,
            body: JSON.stringify({ error: "revision not yet visible" }),
            contentType: "application/json",
        };
    const ctx = { rev, ip, now: now ?? Date.now() };
    const key = `${user}|${action}|${object}|${ip || ""}|${Math.floor(ctx.now / 60000)}`;
    stats.checks++;
    const hit = cache.get(key);
    if (hit && hit.rev >= (atLeast || 0)) {
        stats.cacheHits++;
        return { allowed: hit.allowed, revision: hit.rev, cached: true };
    }
    const r = checkRelation(object, PERMISSION[action], user, ctx);
    cache.put(key, { allowed: r.allowed, rev });
    if (!r.allowed) stats.denied++;
    log.info({ caller: req.caller.sub, user, action, object, allowed: r.allowed, revision: rev }, "decision");
    return { allowed: r.allowed, revision: rev, cached: false };
});
app.post("/expand", (req) => {
    const { user, action, object, ip, now } = JSON.parse(req.body);
    const r = checkRelation(object, PERMISSION[action], user, {
        rev: revision(),
        ip,
        now: now ?? Date.now(),
        explain: true,
    });
    return { allowed: r.allowed, explanation: render(r.node) };
});
app.get("/lookup", (req) => ({
    objects: lookup(req.query.type, req.query.permission, req.query.user, {
        rev: revision(),
        ip: req.query.ip,
        now: Date.now(),
    }),
}));
app.start();

// =============================================================================
// An organisation
// =============================================================================
const base = `http://127.0.0.1:${app.port}`;
const writerToken = JWTSign({ sub: "docs-backend", scopes: ["write", "check"] }, SERVICE_KEY, { alg: "HS256" });
const readerToken = JWTSign({ sub: "search-indexer", scopes: ["check"] }, SERVICE_KEY, { alg: "HS256" });
const post = async (path, body, token = writerToken) => {
    const r = await fetch(base + path, {
        method: "POST",
        body: JSON.stringify(body),
        headers: { authorization: `Bearer ${token}` },
    });
    return { status: r.status, ...(await r.json()) };
};
const can = async (user, action, object, extra = {}) =>
    (await post("/check", { user, action, object, ...extra })).allowed;

const r1 = await post("/write", {
    updates: [
        "group:eng#member@user:alice",
        "group:eng#member@user:bob",
        "group:platform#member@user:carol",
        "group:eng#member@group:platform#member",
        "group:all#member@group:eng#member",
        "group:all#member@user:erin",
        "folder:company#viewer@group:all#member",
        "folder:company#owner@user:ceo",
        "folder:engineering#parent@folder:company",
        "folder:engineering#editor@group:eng#member",
        "document:roadmap#parent@folder:engineering",
        "document:roadmap#owner@user:alice",
        "document:salaries#parent@folder:company",
        "document:salaries#owner@user:ceo",
        "document:salaries#banned@user:erin",
        "document:handbook#parent@folder:company",
        "document:public-faq#viewer@user:*",
    ].map((tuple) => ({ op: "add", tuple })),
});
await post("/write", {
    updates: [
        { op: "add", tuple: "document:roadmap#commenter@user:dave", caveat: { network: "10.8.0.0/16" } },
        { op: "add", tuple: "document:handbook#editor@user:frank", caveat: { notAfter: "2026-06-05T17:00:00Z" } },
    ],
});

const results = {};
const JUNE3 = Date.UTC(2026, 5, 3, 12),
    JUNE8 = Date.UTC(2026, 5, 8, 12);
results["nested group (platform ⊂ eng) inherits folder editor rights"] = await can(
    "user:carol",
    "write",
    "document:roadmap",
);
results["company-wide viewers read but cannot edit"] =
    (await can("user:erin", "read", "document:handbook")) && !(await can("user:erin", "write", "document:handbook"));
results["an explicit ban subtracts from inherited view rights"] =
    !(await can("user:erin", "read", "document:salaries")) && (await can("user:bob", "read", "document:salaries"));
results["a network caveat allows from the VPN only"] =
    (await can("user:dave", "comment", "document:roadmap", { ip: "10.8.3.7" })) &&
    !(await can("user:dave", "comment", "document:roadmap", { ip: "203.0.113.9" }));
results["a time-boxed grant expires"] =
    (await can("user:frank", "write", "document:handbook", { now: JUNE3 })) &&
    !(await can("user:frank", "write", "document:handbook", { now: JUNE8 }));
results["public wildcard grants are scoped to one document"] =
    (await can("user:nobody", "read", "document:public-faq")) &&
    !(await can("user:nobody", "read", "document:roadmap"));

// Consistency: revoke bob's access; a check carrying the write's token must not hit the stale cache entry.
results["bob can edit before the change"] = await can("user:bob", "write", "document:roadmap");
const rRevoke = await post("/write", { updates: [{ op: "delete", tuple: "group:eng#member@user:bob" }] });
const stale = await post("/check", { user: "user:bob", action: "write", object: "document:roadmap" });
const fresh = await post("/check", {
    user: "user:bob",
    action: "write",
    object: "document:roadmap",
    atLeast: rRevoke.revision,
});
results["a check at the revoke's revision bypasses the stale cached allow (new-enemy problem)"] =
    stale.cached === true && stale.allowed === true && fresh.allowed === false && fresh.cached === false;
const future = await post("/check", {
    user: "user:bob",
    action: "read",
    object: "document:roadmap",
    atLeast: rRevoke.revision + 5,
});
results["a token from the future is refused, not silently answered"] = future.status === 409;

// Explanations and lookups.
const why = await post("/expand", { user: "user:carol", action: "write", object: "document:roadmap" });
results["the explanation names the folder and the nested group"] =
    why.allowed && /group:platform#member/.test(why.explanation) && /folder:engineering/.test(why.explanation);
const visible = (
    await (
        await fetch(`${base}/lookup?type=document&permission=read&user=user:erin`, {
            headers: { authorization: `Bearer ${readerToken}` },
        })
    ).json()
).objects;
results[`lookup lists exactly what erin can read (${visible})`] =
    visible.join() === "document:handbook,document:public-faq,document:roadmap";

// Safety: cycles, malformed writes, missing/insufficient tokens.
await post("/write", { updates: [{ op: "add", tuple: "group:platform#member@group:all#member" }] }); // all ⊃ eng ⊃ platform ⊃ all
results["a group cycle terminates and still answers correctly"] =
    !(await can("user:mallory", "read", "document:roadmap")) && (await can("user:carol", "read", "document:roadmap"));
await post("/write", { updates: [{ op: "delete", tuple: "group:platform#member@group:all#member" }] });
results["a relation missing from the schema is refused"] =
    (await post("/write", { updates: [{ op: "add", tuple: "document:x#approver@user:a" }] })).status === 400;
results["a caller without the write scope cannot write"] =
    (await post("/write", { updates: [{ op: "add", tuple: "document:x#viewer@user:a" }] }, readerToken)).status === 403;
results["unauthenticated callers are refused"] =
    (await fetch(base + "/check", { method: "POST", body: "{}" })).status === 401;

// Policy test suite in YAML, as a team would keep beside the schema.
const suite = YAML.Parse(`
- { user: user:alice, action: share, object: document:roadmap, expect: allow }
- { user: user:carol, action: share, object: document:roadmap, expect: deny }
- { user: user:ceo, action: write, object: document:roadmap, expect: allow }
- { user: user:erin, action: comment, object: document:roadmap, expect: deny }
- { user: user:bob, action: read, object: document:roadmap, expect: deny }
- { user: user:carol, action: read, object: document:salaries, expect: allow }
`);
const suiteRows = [];
for (const t of suite) {
    const got = (await can(t.user, t.action, t.object)) ? "allow" : "deny";
    suiteRows.push([t.user, t.action, t.object, t.expect, got, got === t.expect ? "pass" : "FAIL"]);
}
results["the YAML policy suite passes"] = suiteRows.every((r) => r[5] === "pass");
const everyone = [
    "user:alice",
    "user:bob",
    "user:carol",
    "user:dave",
    "user:erin",
    "user:frank",
    "user:ceo",
    "user:mallory",
];
const docs = ["document:roadmap", "document:salaries", "document:handbook", "document:public-faq"];
const disagreements = everyone.flatMap((u) =>
    docs.flatMap((d) =>
        Object.values(PERMISSION).filter((rel) => {
            const ctx = { rev: revision(), now: Date.now() };
            return (
                checkRelation(d, rel, u, ctx).allowed !== checkRelation(d, rel, u, { ...ctx, explain: true }).allowed
            );
        }),
    ),
);
results[
    `Leopard-indexed checks agree with full recursion on all ${everyone.length * docs.length * 4} user/object/relation triples (${leopardHits} bitmap probes)`
] = disagreements.length === 0 && leopardHits > 0;
results[`${audit.length} audited decisions carry caller and revision`] =
    audit.length > 0 && audit.every((e) => e.revision !== undefined && e.caller);

print("access-control");
print(
    `  tuples     ${db.query("SELECT COUNT(*) AS n FROM tuples")[0].n} written over ${revision()} revisions (initial write r${r1.revision})`,
);
print(`  checks     ${stats.checks} (${stats.cacheHits} from cache, ${stats.denied} denied)`);
print("  why can carol write document:roadmap?");
print(
    why.explanation
        .split("\n")
        .slice(0, 8)
        .map((l) => "    " + l)
        .join("\n"),
);
print(table([["user", "action", "object", "expect", "got", ""], ...suiteRows]));
app.close();
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
