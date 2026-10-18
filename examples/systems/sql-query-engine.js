// SQL query engine — a parser, planner and executor for a useful SQL subset, with every query cross-checked against
// SQLite running the same text over the same rows.
//
// WHAT IT IS
//   The front half of a database, small enough to read in one sitting:
//
//   parser      tokenizer + recursive-descent parser for SELECT [DISTINCT] ... FROM ... [JOIN ... ON a = b]
//               [WHERE] [GROUP BY] [HAVING] [ORDER BY ... ASC|DESC] [LIMIT]; expressions with AND/OR/NOT,
//               comparisons, BETWEEN, IN, LIKE, IS [NOT] NULL, arithmetic and COUNT/SUM/AVG/MIN/MAX
//   semantics   SQL three-valued logic (NULL comparisons are unknown, WHERE keeps only true), NULLs group
//               together and sort first, COUNT(col) skips NULLs, LIKE is ASCII case-insensitive — SQLite's rules
//   planner     conjuncts that touch one table are pushed below the join; a range or equality on an indexed
//               column becomes an index scan over a SortedMap (amount) or a Multimap (status); equi-joins are
//               hash joins on a native Multimap; ORDER BY + LIMIT keeps a bounded Heap (comparator-ordered) instead of sorting
//   explain     every query prints its plan; the checks assert which access path was chosen
//   oracle      the same SQL runs in SQLite (dyna:net) over identical rows; result sets must be equal
//
// MODULES  dyna:net (SQLite) · dyna:structures (SortedMap, Multimap, Heap) · dyna:random · dyna:cli ·
//          Array extensions (sortWith, groupBy, unique)
//
// RUN      dynajs examples/systems/sql-query-engine.js

import { SQLite } from "dyna:net";
import { SortedMap, Multimap, Heap } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1979);

// =============================================================================
// Data
// =============================================================================
const COUNTRIES = ["DE", "FR", "US", "JP", "IN", "BR"],
    TIERS = ["gold", "silver", "bronze", null];
const STATUSES = ["paid", "shipped", "refunded", "cancelled", "pending"];
const tables = {
    customers: Array.from({ length: 2000 }, (_, i) => ({
        id: i + 1,
        name: `cust_${(i * 7919) % 10007}`,
        country: COUNTRIES[i % 6],
        tier: TIERS[(i * 13) % 4],
    })),
    orders: Array.from({ length: 40_000 }, (_, i) => ({
        id: i + 1,
        customer_id: 1 + rng.nextBounded(2100),
        amount: Math.round(rng.exponential(1 / 80) * 100) / 100,
        status: STATUSES[Math.min(4, Math.floor(rng.exponential(1.2)))],
        day: rng.nextBounded(365),
    })),
};
const schema = {
    customers: ["id", "name", "country", "tier"],
    orders: ["id", "customer_id", "amount", "status", "day"],
};

// =============================================================================
// Tokenizer and parser
// =============================================================================
const KEYWORDS = new Set(
    "SELECT DISTINCT FROM AS JOIN ON WHERE GROUP BY HAVING ORDER ASC DESC LIMIT AND OR NOT BETWEEN IN LIKE IS NULL COUNT SUM AVG MIN MAX".split(
        " ",
    ),
);
function tokenize(sql) {
    const out = [],
        re = /\s*(?:(\d+(?:\.\d+)?)|('(?:[^']|'')*')|([A-Za-z_][\w]*)|(<=|>=|<>|!=|[=<>(),.*+\-/]))/y;
    let m;
    while (re.lastIndex < sql.length && (m = re.exec(sql))) {
        if (m[1]) out.push({ t: "num", v: Number(m[1]) });
        else if (m[2]) out.push({ t: "str", v: m[2].slice(1, -1).replaceAll("''", "'") });
        else if (m[3])
            out.push(KEYWORDS.has(m[3].toUpperCase()) ? { t: "kw", v: m[3].toUpperCase() } : { t: "id", v: m[3] });
        else out.push({ t: "op", v: m[4] });
    }
    if (re.lastIndex < sql.trimEnd().length)
        throw new SyntaxError(`unexpected input at ${re.lastIndex}: ${sql.slice(re.lastIndex, re.lastIndex + 10)}`);
    return out;
}
function parse(sql) {
    const toks = tokenize(sql);
    let p = 0;
    const peek = (v) => toks[p] && toks[p].v === v;
    const take = (v) => {
        if (!peek(v)) throw new SyntaxError(`expected ${v} at token ${p} (${toks[p]?.v ?? "end"})`);
        return toks[p++];
    };
    const maybe = (v) => (peek(v) ? (p++, true) : false);
    function primary() {
        const tk = toks[p];
        if (!tk) throw new SyntaxError("unexpected end");
        if (tk.t === "num" || tk.t === "str") {
            p++;
            return { k: "lit", v: tk.v };
        }
        if (tk.v === "NULL") {
            p++;
            return { k: "lit", v: null };
        }
        if (tk.v === "(") {
            p++;
            const e = expr();
            take(")");
            return e;
        }
        if (tk.v === "-") {
            p++;
            return { k: "neg", e: primary() };
        }
        if (["COUNT", "SUM", "AVG", "MIN", "MAX"].includes(tk.v)) {
            p++;
            take("(");
            const star = maybe("*");
            const arg = star ? null : expr();
            take(")");
            return { k: "agg", fn: tk.v, arg };
        }
        if (tk.t === "id") {
            p++;
            if (maybe(".")) return { k: "col", table: tk.v, name: toks[p++].v };
            return { k: "col", table: null, name: tk.v };
        }
        throw new SyntaxError(`unexpected ${tk.v}`);
    }
    function mul() {
        let e = primary();
        while (peek("*") || peek("/")) {
            const op = toks[p++].v;
            e = { k: "bin", op, a: e, b: primary() };
        }
        return e;
    }
    function addSub() {
        let e = mul();
        while (peek("+") || peek("-")) {
            const op = toks[p++].v;
            e = { k: "bin", op, a: e, b: mul() };
        }
        return e;
    }
    function cmp() {
        const a = addSub();
        if (maybe("IS")) {
            const not = maybe("NOT");
            take("NULL");
            return { k: "isnull", e: a, not };
        }
        const not = maybe("NOT");
        if (maybe("BETWEEN")) {
            const lo = addSub();
            take("AND");
            return { k: "between", e: a, lo, hi: addSub(), not };
        }
        if (maybe("IN")) {
            take("(");
            const list = [addSub()];
            while (maybe(",")) list.push(addSub());
            take(")");
            return { k: "in", e: a, list, not };
        }
        if (maybe("LIKE")) return { k: "like", e: a, pat: addSub(), not };
        for (const op of ["=", "!=", "<>", "<=", ">=", "<", ">"])
            if (maybe(op)) return { k: "bin", op: op === "<>" ? "!=" : op, a, b: addSub() };
        return a;
    }
    function notExpr() {
        return maybe("NOT") ? { k: "not", e: notExpr() } : cmp();
    }
    function andExpr() {
        let e = notExpr();
        while (maybe("AND")) e = { k: "and", a: e, b: notExpr() };
        return e;
    }
    function expr() {
        let e = andExpr();
        while (maybe("OR")) e = { k: "or", a: e, b: andExpr() };
        return e;
    }

    take("SELECT");
    const q = { distinct: maybe("DISTINCT"), items: [], joins: [] };
    do {
        if (maybe("*")) {
            q.items.push({ star: true });
            continue;
        }
        const e = expr();
        q.items.push({ e, alias: maybe("AS") ? toks[p++].v : null });
    } while (maybe(","));
    take("FROM");
    const ref = () => {
        const name = toks[p++].v;
        const alias = maybe("AS") || toks[p]?.t === "id" ? toks[p++].v : name;
        return { name, alias };
    };
    q.from = ref();
    while (maybe("JOIN")) {
        const r = ref();
        take("ON");
        q.joins.push({ ...r, on: expr() });
    }
    if (maybe("WHERE")) q.where = expr();
    if (maybe("GROUP")) {
        take("BY");
        q.groupBy = [expr()];
        while (maybe(",")) q.groupBy.push(expr());
    }
    if (maybe("HAVING")) q.having = expr();
    if (maybe("ORDER")) {
        take("BY");
        q.orderBy = [];
        do {
            const e = expr();
            q.orderBy.push({ e, desc: maybe("DESC") ? true : (maybe("ASC"), false) });
        } while (maybe(","));
    }
    if (maybe("LIMIT")) q.limit = toks[p++].v;
    if (p !== toks.length) throw new SyntaxError(`trailing input at token ${p}: ${toks[p].v}`);
    return q;
}

// =============================================================================
// Evaluation with SQL NULL semantics
// =============================================================================
const isNull = (v) => v === null || v === undefined;
function evaluate(e, row, aggs) {
    switch (e.k) {
        case "lit":
            return e.v;
        case "col":
            return row[e.key];
        case "neg": {
            const v = evaluate(e.e, row, aggs);
            return isNull(v) ? null : -v;
        }
        case "agg":
            return aggs.get(e);
        case "bin": {
            const a = evaluate(e.a, row, aggs),
                b = evaluate(e.b, row, aggs);
            if (isNull(a) || isNull(b)) return null;
            switch (e.op) {
                case "+":
                    return a + b;
                case "-":
                    return a - b;
                case "*":
                    return a * b;
                case "/":
                    return b === 0 ? null : a / b;
                case "=":
                    return a === b;
                case "!=":
                    return a !== b;
                case "<":
                    return a < b;
                case "<=":
                    return a <= b;
                case ">":
                    return a > b;
                case ">=":
                    return a >= b;
            }
            throw new Error(e.op);
        }
        case "and": {
            const a = evaluate(e.a, row, aggs),
                b = evaluate(e.b, row, aggs);
            return a === false || b === false ? false : isNull(a) || isNull(b) ? null : true;
        }
        case "or": {
            const a = evaluate(e.a, row, aggs),
                b = evaluate(e.b, row, aggs);
            return a === true || b === true ? true : isNull(a) || isNull(b) ? null : false;
        }
        case "not": {
            const v = evaluate(e.e, row, aggs);
            return isNull(v) ? null : !v;
        }
        case "isnull":
            return isNull(evaluate(e.e, row, aggs)) !== e.not;
        case "between": {
            const v = evaluate(e.e, row, aggs),
                lo = evaluate(e.lo, row, aggs),
                hi = evaluate(e.hi, row, aggs);
            if (isNull(v) || isNull(lo) || isNull(hi)) return null;
            return (v >= lo && v <= hi) !== e.not;
        }
        case "in": {
            const v = evaluate(e.e, row, aggs);
            if (isNull(v)) return null;
            const vals = e.list.map((x) => evaluate(x, row, aggs));
            if (vals.includes(v)) return !e.not;
            return vals.some(isNull) ? null : e.not;
        }
        case "like": {
            const v = evaluate(e.e, row, aggs),
                pat = evaluate(e.pat, row, aggs);
            if (isNull(v) || isNull(pat)) return null;
            const re = e.pat.k === "lit" ? (e.re ??= likeRegExp(pat)) : likeRegExp(pat);
            return re.test(String(v)) !== e.not;
        }
    }
    throw new Error(`cannot evaluate ${e.k}`);
}
function likeRegExp(pat) {
    return new RegExp(
        "^" +
            [...String(pat)]
                .map((c) => (c === "%" ? ".*" : c === "_" ? "." : c.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")))
                .join("") +
            "$",
        "is",
    );
}
const walk = (e, fn) => {
    if (!e || typeof e !== "object") return;
    fn(e);
    for (const k of ["a", "b", "e", "lo", "hi", "pat", "arg"]) walk(e[k], fn);
    (e.list || []).forEach((x) => walk(x, fn));
};
const tablesIn = (e, q) => {
    const out = new Set();
    walk(e, (n) => {
        if (n.k !== "col") return;
        if (n.table) out.add(n.table);
        else for (const r of [q.from, ...q.joins]) if (schema[r.name].includes(n.name)) out.add(r.alias);
    });
    return out;
};
const conjuncts = (e) => (!e ? [] : e.k === "and" ? [...conjuncts(e.a), ...conjuncts(e.b)] : [e]);
// SQLite ordering: NULL < numbers < text.
const typeRank = (v) => (isNull(v) ? 0 : typeof v === "number" || typeof v === "boolean" ? 1 : 2);
const sqlCompare = (a, b) => typeRank(a) - typeRank(b) || (a < b ? -1 : a > b ? 1 : 0);

// =============================================================================
// Indexes and planner
// =============================================================================
const amountIndex = new SortedMap(),
    statusIndex = new Multimap();
for (const o of tables.orders) {
    const cents = Math.round(o.amount * 100);
    const bucket = amountIndex.get(cents);
    if (bucket) bucket.push(o);
    else amountIndex.set(cents, [o]);
    statusIndex.put(o.status, o);
}
function accessPath(ref, preds) {
    if (ref.name !== "orders") return { kind: "seq", rows: tables[ref.name], used: null };
    for (const c of preds) {
        const col = c.e ?? c.a;
        if (col?.k !== "col" || (col.name !== "amount" && col.name !== "status")) continue;
        if (c.k === "between" && !c.not && col.name === "amount" && c.lo.k === "lit" && c.hi.k === "lit")
            return {
                kind: `index range amount [${c.lo.v}, ${c.hi.v}]`,
                rows: amountIndex.rangeQuery(Math.ceil(c.lo.v * 100), Math.floor(c.hi.v * 100)).flatMap(([, rs]) => rs),
                used: c,
            };
        if (c.k === "bin" && [">=", ">"].includes(c.op) && col.name === "amount" && c.b.k === "lit")
            return {
                kind: `index range amount ${c.op} ${c.b.v}`,
                rows: amountIndex.rangeQuery(Math.ceil(c.b.v * 100), Infinity).flatMap(([, rs]) => rs),
                used: null,
            };
        if (c.k === "bin" && c.op === "=" && col.name === "status" && c.b.k === "lit")
            return { kind: `index lookup status = '${c.b.v}'`, rows: statusIndex.get(c.b.v), used: c };
        if (c.k === "in" && !c.not && col.name === "status" && c.list.every((x) => x.k === "lit"))
            return {
                kind: `index lookup status IN (${c.list.length})`,
                rows: c.list.flatMap((x) => statusIndex.get(x.v)),
                used: c,
            };
    }
    return { kind: "seq", rows: tables[ref.name], used: null };
}
function qualify(ref, row) {
    const out = {};
    for (const k of schema[ref.name]) out[`${ref.alias}.${k}`] = row[k];
    return out;
}

// Binds every column reference to its qualified row key once, so evaluation is one property read.
function bind(q) {
    const refs = [q.from, ...q.joins];
    const visit = (e) =>
        walk(e, (n) => {
            if (n.k !== "col") return;
            const owner = n.table
                ? refs.find((r) => r.alias === n.table)
                : refs.find((r) => schema[r.name].includes(n.name));
            if (owner) {
                n.table = owner.alias;
                n.key = `${owner.alias}.${n.name}`;
            }
        });
    q.items.forEach((it) => visit(it.e));
    q.joins.forEach((j) => visit(j.on));
    [q.where, q.having].forEach(visit);
    (q.groupBy ?? []).forEach(visit);
    (q.orderBy ?? []).forEach((o) => visit(o.e));
    return q;
}

function execute(sql) {
    const q = bind(parse(sql)),
        plan = [];
    const preds = conjuncts(q.where);
    const local = new Map(),
        residual = [];
    for (const c of preds) {
        const ts = tablesIn(c, q);
        if (ts.size === 1) {
            const t = [...ts][0];
            (local.get(t) ?? local.set(t, []).get(t)).push(c);
        } else residual.push(c);
    }
    const scan = (ref) => {
        const mine = local.get(ref.alias) ?? [];
        const path = accessPath(ref, mine);
        const rest = mine.filter((c) => c !== path.used);
        plan.push(`${path.kind} on ${ref.name} AS ${ref.alias}${rest.length ? ` filter(${rest.length})` : ""}`);
        const out = [];
        for (const r of path.rows) {
            const row = qualify(ref, r);
            if (rest.every((c) => evaluate(c, row) === true)) out.push(row);
        }
        return out;
    };
    let rows = scan(q.from);
    for (const j of q.joins) {
        const right = scan(j);
        const on = j.on;
        const [lk, rk] = tablesIn(on.a, q).has(j.alias) ? [on.b, on.a] : [on.a, on.b];
        const build = new Multimap();
        for (const r of right) {
            const key = evaluate(rk, r);
            if (!isNull(key)) build.put(String(key), r);
        }
        plan.push(`hash join ON ${lk.table}.${lk.name} = ${rk.table}.${rk.name} (build ${right.length} rows)`);
        const joined = [];
        for (const l of rows) {
            const key = evaluate(lk, l);
            if (isNull(key)) continue;
            for (const r of build.get(String(key))) joined.push({ ...l, ...r });
        }
        rows = joined;
    }
    if (residual.length) {
        plan.push(`filter(${residual.length}) after join`);
        rows = rows.filter((r) => residual.every((c) => evaluate(c, r) === true));
    }

    const aggNodes = [];
    for (const it of q.items) walk(it.e, (n) => n.k === "agg" && aggNodes.push(n));
    walk(q.having, (n) => n.k === "agg" && aggNodes.push(n));
    for (const o of q.orderBy ?? []) walk(o.e, (n) => n.k === "agg" && aggNodes.push(n));
    let groups;
    if (q.groupBy || aggNodes.length) {
        const byKey = new Map();
        for (const r of rows) {
            const key = JSON.stringify((q.groupBy ?? []).map((g) => evaluate(g, r) ?? null));
            (byKey.get(key) ?? byKey.set(key, []).get(key)).push(r);
        }
        if (!q.groupBy && byKey.size === 0) byKey.set("[]", []);
        plan.push(`hash aggregate ${byKey.size} groups, ${aggNodes.length} aggregates`);
        groups = [...byKey.values()].map((members) => {
            const aggs = new Map();
            for (const a of aggNodes) {
                const vals = a.arg ? members.map((r) => evaluate(a.arg, r)).filter((v) => !isNull(v)) : members;
                aggs.set(
                    a,
                    a.fn === "COUNT"
                        ? vals.length
                        : !vals.length
                          ? null
                          : a.fn === "SUM"
                            ? vals.sum()
                            : a.fn === "AVG"
                              ? vals.sum() / vals.length
                              : a.fn === "MIN"
                                ? vals.reduce((x, y) => (sqlCompare(y, x) < 0 ? y : x))
                                : vals.reduce((x, y) => (sqlCompare(y, x) > 0 ? y : x)),
                );
            }
            return { row: members[0] ?? {}, aggs };
        });
        if (q.having) groups = groups.filter((g) => evaluate(q.having, g.row, g.aggs) === true);
    } else groups = rows.map((row) => ({ row, aggs: null }));

    const project = (g) => {
        const out = [];
        for (const it of q.items) {
            if (it.star) for (const k of Object.keys(g.row)) out.push(g.row[k]);
            else {
                const v = evaluate(it.e, g.row, g.aggs);
                out.push(typeof v === "boolean" ? Number(v) : (v ?? null));
            }
        }
        return out;
    };
    let out = groups.map((g) => ({ g, vals: project(g) }));
    if (q.distinct) out = out.unique((x) => JSON.stringify(x.vals));
    if (q.orderBy) {
        const keyOf = (x) =>
            q.orderBy.map((o) => {
                const alias = o.e.k === "col" && !o.e.table && q.items.findIndex((it) => it.alias === o.e.name);
                return alias >= 0 && alias !== false ? x.vals[alias] : evaluate(o.e, x.g.row, x.g.aggs);
            });
        const cmp = (a, b) => {
            const ka = a.key,
                kb = b.key;
            for (let i = 0; i < ka.length; i++) {
                const c = sqlCompare(ka[i], kb[i]);
                if (c) return q.orderBy[i].desc ? -c : c;
            }
            return 0;
        };
        out.forEach((x) => (x.key = keyOf(x)));
        if (q.limit !== undefined && q.limit < out.length / 8) {
            const heap = new Heap((a, b) => cmp(b, a));
            for (const x of out) if (heap.push(x) > q.limit) heap.pop();
            plan.push(`top-${q.limit} via bounded heap`);
            const top = [];
            let x;
            while ((x = heap.pop()) !== undefined) top.push(x);
            out = top.reverse();
        } else {
            out.sort(cmp);
            plan.push("sort");
        }
    }
    if (q.limit !== undefined) out = out.slice(0, q.limit);
    return { rows: out.map((x) => x.vals), plan };
}

// =============================================================================
// Oracle: SQLite over the same rows
// =============================================================================
const db = new SQLite(":memory:");
db.exec("CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, country TEXT, tier TEXT)");
db.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, amount REAL, status TEXT, day INTEGER)");
db.exec("BEGIN");
for (const c of tables.customers)
    db.exec("INSERT INTO customers VALUES (?, ?, ?, ?)", [c.id, c.name, c.country, c.tier]);
for (const o of tables.orders)
    db.exec("INSERT INTO orders VALUES (?, ?, ?, ?, ?)", [o.id, o.customer_id, o.amount, o.status, o.day]);
db.exec("COMMIT");

const norm = (v) => (typeof v === "number" && !Number.isInteger(v) ? Math.round(v * 1e6) / 1e6 : v);
const canon = (rows, ordered) => {
    const r = rows.map((x) => JSON.stringify(x.map(norm)));
    return ordered ? r : r.sort();
};

const QUERIES = [
    [
        "revenue by country",
        "SELECT c.country, COUNT(*) AS n, SUM(o.amount) AS revenue FROM orders o JOIN customers c ON o.customer_id = c.id WHERE o.status IN ('paid', 'shipped') GROUP BY c.country ORDER BY revenue DESC",
        "index lookup status IN",
    ],
    [
        "NULL tier forms its own group",
        "SELECT c.tier, COUNT(c.tier) AS counted, COUNT(*) AS n FROM customers c GROUP BY c.tier ORDER BY c.tier",
        "seq",
    ],
    ["three-valued logic drops NULL tiers", "SELECT COUNT(*) AS n FROM customers WHERE NOT (tier = 'gold')", "seq"],
    [
        "index range scan with residual filter",
        "SELECT id, amount FROM orders WHERE amount BETWEEN 250 AND 260 AND day < 100 ORDER BY amount, id",
        "index range amount [250, 260]",
    ],
    [
        "top-N with a bounded heap",
        "SELECT id, amount, status FROM orders WHERE status = 'refunded' ORDER BY amount DESC, id LIMIT 5",
        "top-5 via bounded heap",
    ],
    [
        "HAVING over an aggregate",
        "SELECT o.customer_id, COUNT(*) AS n, MAX(o.amount) AS biggest FROM orders o WHERE o.day BETWEEN 0 AND 30 GROUP BY o.customer_id HAVING COUNT(*) >= 3 ORDER BY n DESC, o.customer_id LIMIT 10",
        "hash aggregate",
    ],
    [
        "orphan orders vanish in an inner join",
        "SELECT COUNT(*) AS n FROM orders o JOIN customers c ON o.customer_id = c.id",
        "hash join",
    ],
    [
        "LIKE is case-insensitive",
        "SELECT COUNT(*) AS n FROM customers WHERE name LIKE 'CUST\\_1%' OR name LIKE 'cust_99__'",
        "seq",
    ],
    [
        "DISTINCT and arithmetic",
        "SELECT DISTINCT c.country, c.tier IS NULL AS untiered FROM customers c WHERE c.id * 2 > 3000 ORDER BY c.country, untiered",
        "seq",
    ],
    [
        "NOT over AND keeps unknown unknown",
        "SELECT COUNT(*) AS n FROM customers WHERE NOT (tier = 'gold' AND country = 'JP') OR NOT (tier = 'bronze' OR country = 'DE')",
        "seq",
    ],
    [
        "IN with NULL is unknown, not false",
        "SELECT COUNT(*) AS n FROM customers WHERE tier NOT IN ('gold', NULL)",
        "seq",
    ],
    [
        "global aggregate over no rows",
        "SELECT COUNT(*) AS n, SUM(amount) AS s, MIN(day) AS d FROM orders WHERE amount > 1000000",
        "index range amount > 1000000",
    ],
    [
        "average basket by tier and status",
        "SELECT c.tier, o.status, AVG(o.amount) AS avg_amount, MIN(o.day) AS first_day FROM orders o JOIN customers c ON c.id = o.customer_id WHERE o.amount >= 20 AND c.country = 'JP' GROUP BY c.tier, o.status ORDER BY c.tier, o.status",
        "index range amount >= 20",
    ],
];
// LIKE 'CUST\_1%' in SQLite has no default escape: the backslash is a literal character; match that.
const report = [];
let allEqual = true,
    plansOk = true;
for (const [name, sql, expectPlan] of QUERIES) {
    const t0 = performance.now(),
        mine = execute(sql),
        t1 = performance.now();
    const theirs = db.query(sql).map((r) => Object.values(r));
    const t2 = performance.now();
    const ordered = /ORDER BY/.test(sql);
    const equal = JSON.stringify(canon(mine.rows, ordered)) === JSON.stringify(canon(theirs, ordered));
    const planHit = mine.plan.some((s) => s.startsWith(expectPlan) || s.includes(expectPlan));
    if (!equal) {
        allEqual = false;
        print(
            `MISMATCH ${name}\n  mine   ${JSON.stringify(mine.rows.slice(0, 3))}\n  sqlite ${JSON.stringify(theirs.slice(0, 3))}`,
        );
    }
    if (!planHit) {
        plansOk = false;
        print(`PLAN ${name}: ${mine.plan.join(" -> ")}`);
    }
    results[`${name}: equals SQLite`] = equal;
    report.push([
        name,
        mine.rows.length,
        (t1 - t0).toFixed(1),
        (t2 - t1).toFixed(1),
        mine.plan.join(" -> ").truncate(70),
    ]);
}
print(Table(report, { head: ["query", "rows", "ms", "sqlite ms", "plan"] }));
results["planner chose the expected access path for every query"] = plansOk;
results["all thirteen queries match"] = allEqual;

// =============================================================================
// Refusals
// =============================================================================
const refused = [
    "SELECT FROM orders",
    "SELECT id FROM orders WHERE",
    "SELECT id FROM orders LIMIT 5 extra",
    "SELECT id FROM orders WHERE amount BETWEEN 1",
    "SELECT 'unterminated FROM orders",
].filter((sql) => {
    try {
        execute(sql);
        return false;
    } catch (e) {
        return e instanceof SyntaxError;
    }
});
results["malformed SQL is refused with SyntaxError"] = refused.length === 5;

db.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
