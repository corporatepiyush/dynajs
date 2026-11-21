// SQL query engine — a parser, planner and executor for a useful SQL subset, with every query cross-checked against
// SQLite running the same text over the same rows.
//
// WHAT IT IS
//   The front half of a database, small enough to read in one sitting:
//
//   parser      tokenizer + recursive-descent parser for SELECT [DISTINCT] ... FROM ... [[LEFT] JOIN ... ON a = b]
//               [WHERE] [GROUP BY] [HAVING] [ORDER BY ... ASC|DESC] [LIMIT]; expressions with AND/OR/NOT,
//               comparisons, BETWEEN, IN (list or uncorrelated SELECT), LIKE, IS [NOT] NULL, CASE WHEN,
//               arithmetic and COUNT/SUM/AVG/MIN/MAX with DISTINCT
//   executor    rows are tuples; every expression compiles once into closures over tuple slots; joins concatenate
//               tuples; aggregation is one pass with an accumulator per aggregate
//   semantics   SQL three-valued logic (NULL comparisons are unknown, WHERE keeps only true), NULLs group
//               together and sort first, COUNT(col) skips NULLs, LIKE is ASCII case-insensitive — SQLite's rules
//   planner     conjuncts that touch one table are pushed below the join; a range or equality on an indexed
//               column becomes an index scan over a SortedMap (amount) or a Multimap (status); equi-joins are
//               hash joins; a WHERE conjunct on the null-extended side of a LEFT JOIN stays above the join;
//               decimal bounds are widened by a hair (0.07 is not exact in binary) and re-checked, so an index
//               range may over-select but never drops a row; ORDER BY + LIMIT keeps a bounded Heap
//   explain     every query prints its plan; the checks assert which access path was chosen
//   oracle      the same SQL runs in SQLite (dyna:sqlite) over identical rows; result sets must be equal: 23 named
//               queries, then 170 generated ones (random predicates with NULL literals, every operator, joins,
//               grouping, subqueries, inexact decimal bounds) compared as multisets
//
// MODULES  dyna:sqlite · dyna:structures (SortedMap, Multimap, Heap) · dyna:random · dyna:cli ·
//          Array extensions (sortWith, groupBy, unique)
//
// RUN      dynajs examples/systems/sql-query-engine.js

import { SQLite } from "dyna:sqlite";
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
// a 3000-row slice of the orders with NULLs sprinkled into amount, status and customer_id: the random queries run here
tables.sample = tables.orders
    .values()
    .take(3000)
    .map((o) => ({
        ...o,
        amount: rng.nextBounded(20) === 0 ? null : o.amount,
        status: rng.nextBounded(30) === 0 ? null : o.status,
        customer_id: rng.nextBounded(40) === 0 ? null : o.customer_id,
    }))
    .toArray();
const schema = {
    customers: ["id", "name", "country", "tier"],
    orders: ["id", "customer_id", "amount", "status", "day"],
    sample: ["id", "customer_id", "amount", "status", "day"],
};
// rows are stored once as tuples in schema order; scans, indexes and joins move tuples, never keyed objects
const tuples = Object.fromEntries(
    Object.entries(tables).map(([name, rows]) => [name, rows.map((r) => schema[name].map((k) => r[k]))]),
);

// =============================================================================
// Tokenizer and parser
// =============================================================================
const KEYWORDS = new Set(
    "SELECT DISTINCT FROM AS JOIN LEFT OUTER ON WHERE GROUP BY HAVING ORDER ASC DESC LIMIT AND OR NOT BETWEEN IN LIKE IS NULL COUNT SUM AVG MIN MAX CASE WHEN THEN ELSE END".split(
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
            const star = maybe("*"),
                distinct = !star && maybe("DISTINCT");
            const arg = star ? null : expr();
            take(")");
            return { k: "agg", fn: tk.v, arg, distinct };
        }
        if (tk.v === "CASE") {
            p++;
            const arms = [];
            while (maybe("WHEN")) {
                const when = expr();
                take("THEN");
                arms.push({ when, then: expr() });
            }
            if (!arms.length) throw new SyntaxError("CASE without WHEN");
            const otherwise = maybe("ELSE") ? expr() : { k: "lit", v: null };
            take("END");
            return { k: "case", arms, otherwise };
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
            if (peek("SELECT")) {
                const sub = select();
                take(")");
                return { k: "insub", e: a, sub, not };
            }
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

    const q = select();
    if (p !== toks.length) throw new SyntaxError(`trailing input at token ${p}: ${toks[p].v}`);
    return q;

    function select() {
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
        for (;;) {
            const left = maybe("LEFT");
            if (left) maybe("OUTER");
            if (!maybe("JOIN")) {
                if (left) throw new SyntaxError("LEFT without JOIN");
                break;
            }
            const r = ref();
            take("ON");
            const on = expr();
            if (on.k !== "bin" || on.op !== "=" || on.a.k !== "col" || on.b.k !== "col")
                throw new SyntaxError("JOIN ... ON supports one column = column");
            q.joins.push({ ...r, on, left });
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
        return q;
    }
}

// =============================================================================
// Expressions compile once into closures over tuple slots, with SQL NULL semantics
// =============================================================================
const isNull = (v) => v === null || v === undefined;
// SQLite ordering: NULL < numbers < text.
const typeRank = (v) => (isNull(v) ? 0 : typeof v === "number" || typeof v === "boolean" ? 1 : 2);
const sqlCompare = (a, b) => typeRank(a) - typeRank(b) || (a < b ? -1 : a > b ? 1 : 0);
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
const BINOPS = {
    "+": (a, b) => a + b,
    "-": (a, b) => a - b,
    "*": (a, b) => a * b,
    "/": (a, b) => (b === 0 ? null : a / b),
    "=": (a, b) => a === b,
    "!=": (a, b) => a !== b,
    "<": (a, b) => a < b,
    "<=": (a, b) => a <= b,
    ">": (a, b) => a > b,
    ">=": (a, b) => a >= b,
};
// compile(e, slotOf) -> (tuple, aggs) => value. slotOf maps a column node to its index in the tuple being read,
// so the same tree compiles against one table's tuple (scan filters) or the joined tuple (everything after).
function compile(e, slotOf) {
    const c = (x) => compile(x, slotOf);
    switch (e.k) {
        case "lit": {
            const v = e.v;
            return () => v;
        }
        case "col": {
            const i = slotOf(e);
            return (r) => r[i];
        }
        case "neg": {
            const f = c(e.e);
            return (r, g) => {
                const v = f(r, g);
                return isNull(v) ? null : -v;
            };
        }
        case "agg": {
            const i = e.slot;
            return (r, g) => g[i];
        }
        case "bin": {
            const fa = c(e.a),
                fb = c(e.b),
                op = BINOPS[e.op];
            return (r, g) => {
                const a = fa(r, g),
                    b = fb(r, g);
                return isNull(a) || isNull(b) ? null : op(a, b);
            };
        }
        case "and": {
            const fa = c(e.a),
                fb = c(e.b);
            return (r, g) => {
                const a = fa(r, g),
                    b = fb(r, g);
                return a === false || b === false ? false : isNull(a) || isNull(b) ? null : true;
            };
        }
        case "or": {
            const fa = c(e.a),
                fb = c(e.b);
            return (r, g) => {
                const a = fa(r, g),
                    b = fb(r, g);
                return a === true || b === true ? true : isNull(a) || isNull(b) ? null : false;
            };
        }
        case "not": {
            const f = c(e.e);
            return (r, g) => {
                const v = f(r, g);
                return isNull(v) ? null : !v;
            };
        }
        case "isnull": {
            const f = c(e.e),
                not = e.not;
            return (r, g) => isNull(f(r, g)) !== not;
        }
        case "between": {
            const f = c(e.e),
                flo = c(e.lo),
                fhi = c(e.hi),
                not = e.not;
            return (r, g) => {
                const v = f(r, g),
                    lo = flo(r, g),
                    hi = fhi(r, g);
                // NULL bounds: SQLite evaluates v >= lo AND v <= hi, so one false comparison still decides it
                const ge = isNull(v) || isNull(lo) ? null : v >= lo,
                    le = isNull(v) || isNull(hi) ? null : v <= hi;
                const both = ge === false || le === false ? false : ge === null || le === null ? null : true;
                return both === null ? null : both !== not;
            };
        }
        case "in": {
            const f = c(e.e),
                fs = e.list.map(c),
                not = e.not;
            return (r, g) => {
                const v = f(r, g);
                if (isNull(v)) return null;
                let sawNull = false;
                for (const fx of fs) {
                    const x = fx(r, g);
                    if (isNull(x)) sawNull = true;
                    else if (x === v) return !not;
                }
                return sawNull ? null : not;
            };
        }
        case "insub": {
            // uncorrelated: run once, keep a Set of the first column; an empty result is false even for NULL
            const f = c(e.e),
                not = e.not,
                vals = execute(e.sub).rows.map((row) => row[0]),
                set = new Set(vals.filter((v) => !isNull(v))),
                sawNull = vals.some(isNull),
                empty = vals.length === 0;
            return (r, g) => {
                if (empty) return not;
                const v = f(r, g);
                if (isNull(v)) return null;
                if (set.has(v)) return !not;
                return sawNull ? null : not;
            };
        }
        case "like": {
            const f = c(e.e),
                fp = c(e.pat),
                not = e.not,
                fixed = e.pat.k === "lit" && !isNull(e.pat.v) ? likeRegExp(e.pat.v) : null;
            return (r, g) => {
                const v = f(r, g),
                    pat = fixed ?? fp(r, g);
                if (isNull(v) || isNull(pat)) return null;
                return (fixed ?? likeRegExp(pat)).test(String(v)) !== not;
            };
        }
        case "case": {
            const arms = e.arms.map((a) => [c(a.when), c(a.then)]),
                fo = c(e.otherwise);
            return (r, g) => {
                for (const [w, t] of arms) if (w(r, g) === true) return t(r, g);
                return fo(r, g);
            };
        }
    }
    throw new Error(`cannot compile ${e.k}`);
}
const walk = (e, fn) => {
    if (!e || typeof e !== "object") return;
    fn(e);
    for (const k of ["a", "b", "e", "lo", "hi", "pat", "arg", "otherwise"]) walk(e[k], fn);
    (e.list || []).forEach((x) => walk(x, fn));
    (e.arms || []).forEach((x) => (walk(x.when, fn), walk(x.then, fn)));
};
const conjuncts = (e) => (!e ? [] : e.k === "and" ? [...conjuncts(e.a), ...conjuncts(e.b)] : [e]);

// =============================================================================
// Indexes and planner
// =============================================================================
const AMOUNT = schema.orders.indexOf("amount"),
    STATUS = schema.orders.indexOf("status");
const amountIndex = new SortedMap(),
    statusIndex = new Multimap();
for (const t of tuples.orders) {
    const cents = Math.round(t[AMOUNT] * 100);
    const bucket = amountIndex.get(cents);
    if (bucket) bucket.push(t);
    else amountIndex.set(cents, [t]);
    statusIndex.put(t[STATUS], t);
}
// amounts are cents; a literal like 0.07 is 7.000000000000001 cents, so the bounds widen by a hair and the
// conjunct stays as a residual filter: an index range may over-select, never under-select
const centsFrom = (v) => Math.ceil(v * 100 - 1e-6),
    centsTo = (v) => Math.floor(v * 100 + 1e-6);
function accessPath(ref, preds) {
    if (ref.name !== "orders") return { kind: "seq", rows: tuples[ref.name] };
    const range = (lo, hi) => amountIndex.rangeQuery(lo, hi).flatMap(([, rs]) => rs);
    for (const c of preds) {
        const col = c.e ?? c.a;
        if (col?.k !== "col" || (col.name !== "amount" && col.name !== "status")) continue;
        const lit = c.b?.k === "lit" && typeof c.b.v === "number" ? c.b.v : undefined;
        if (c.k === "between" && !c.not && col.name === "amount" && c.lo.k === "lit" && c.hi.k === "lit")
            if (typeof c.lo.v === "number" && typeof c.hi.v === "number")
                return {
                    kind: `index range amount [${c.lo.v}, ${c.hi.v}]`,
                    rows: range(centsFrom(c.lo.v), centsTo(c.hi.v)),
                };
        if (c.k === "bin" && col.name === "amount" && lit !== undefined) {
            if (c.op === ">=" || c.op === ">")
                return { kind: `index range amount ${c.op} ${lit}`, rows: range(centsFrom(lit), Infinity) };
            if (c.op === "<=" || c.op === "<")
                return { kind: `index range amount ${c.op} ${lit}`, rows: range(-Infinity, centsTo(lit)) };
        }
        if (c.k === "bin" && c.op === "=" && col.name === "status" && c.b.k === "lit")
            return { kind: `index lookup status = '${c.b.v}'`, rows: statusIndex.get(c.b.v), used: c };
        if (c.k === "in" && !c.not && col.name === "status" && c.list.every((x) => x.k === "lit"))
            return {
                kind: `index lookup status IN (${c.list.length})`,
                rows: c.list.unique((x) => x.v).flatMap((x) => statusIndex.get(x.v)),
                used: c,
            };
    }
    return { kind: "seq", rows: tuples[ref.name] };
}

// Binds every column to its table and its slot in the joined tuple: refs are laid out in FROM/JOIN order.
function bind(q) {
    const refs = [q.from, ...q.joins];
    let base = 0;
    for (const r of refs) {
        if (!schema[r.name]) throw new SyntaxError(`no such table: ${r.name}`);
        r.base = base;
        base += schema[r.name].length;
    }
    q.width = base;
    q.refs = refs;
    const visit = (e) =>
        walk(e, (n) => {
            if (n.k !== "col") return;
            const owner = n.table
                ? refs.find((r) => r.alias === n.table)
                : refs.find((r) => schema[r.name].includes(n.name));
            if (!owner || !schema[owner.name].includes(n.name))
                throw new SyntaxError(`no such column: ${n.table ? n.table + "." : ""}${n.name}`);
            n.table = owner.alias;
            n.ref = owner;
            n.index = schema[owner.name].indexOf(n.name);
        });
    q.items.forEach((it) => visit(it.e));
    q.joins.forEach((j) => visit(j.on));
    [q.where, q.having].forEach(visit);
    // GROUP BY may name an output column by its alias (SQLite allows it): group by that expression
    q.groupBy = q.groupBy?.map((g) => {
        const at = g.k === "col" && !g.table ? q.items.findIndex((it) => it.alias === g.name) : -1;
        return at >= 0 ? q.items[at].e : g;
    });
    (q.groupBy ?? []).forEach(visit);
    for (const o of q.orderBy ?? []) {
        // ORDER BY may name an output column by its alias
        const alias = o.e.k === "col" && !o.e.table ? q.items.findIndex((it) => it.alias === o.e.name) : -1;
        if (alias >= 0) o.output = alias;
        else visit(o.e);
    }
    return q;
}
const globalSlot = (n) => n.ref.base + n.index,
    localSlot = (n) => n.index;
const refsOf = (e) => {
    const out = new Set();
    walk(e, (n) => n.k === "col" && out.add(n.ref));
    return out;
};

function execute(sqlOrQuery) {
    const q = typeof sqlOrQuery === "string" ? bind(parse(sqlOrQuery)) : bind(sqlOrQuery),
        plan = [];
    // a WHERE conjunct on one table runs inside that table's scan, except on the null-extended side of a LEFT
    // JOIN: there it must see the NULLs the join adds, so it stays above the join
    const nullable = new Set(q.joins.filter((j) => j.left)),
        local = new Map(),
        residual = [];
    for (const c of conjuncts(q.where)) {
        const rs = refsOf(c);
        const only = rs.size === 1 ? [...rs][0] : null;
        if (only && !nullable.has(only)) (local.get(only) ?? local.set(only, []).get(only)).push(c);
        else residual.push(c);
    }
    const scan = (ref) => {
        const mine = local.get(ref) ?? [];
        const path = accessPath(ref, mine);
        const rest = mine.filter((c) => c !== path.used).map((c) => compile(c, localSlot));
        plan.push(`${path.kind} on ${ref.name} AS ${ref.alias}${rest.length ? ` filter(${rest.length})` : ""}`);
        if (!rest.length) return path.rows;
        return path.rows.filter((t) => rest.every((f) => f(t) === true));
    };
    let rows = scan(q.from);
    for (const j of q.joins) {
        const right = scan(j);
        const { a, b } = j.on;
        const [lk, rk] = a.ref === j ? [b, a] : [a, b];
        const rkey = compile(rk, localSlot),
            lkey = compile(lk, globalSlot);
        const build = new Map();
        for (const r of right) {
            const key = rkey(r);
            if (isNull(key)) continue;
            const bucket = build.get(key);
            if (bucket) bucket.push(r);
            else build.set(key, [r]);
        }
        plan.push(
            `${j.left ? "left " : ""}hash join ON ${lk.table}.${lk.name} = ${rk.table}.${rk.name} (build ${right.length} rows)`,
        );
        const joined = [],
            nulls = new Array(schema[j.name].length).fill(null);
        for (const l of rows) {
            const key = lkey(l);
            const matches = isNull(key) ? undefined : build.get(key);
            if (matches) for (const r of matches) joined.push(l.concat(r));
            else if (j.left) joined.push(l.concat(nulls));
        }
        rows = joined;
    }
    if (residual.length) {
        plan.push(`filter(${residual.length}) after join`);
        const fs = residual.map((c) => compile(c, globalSlot));
        rows = rows.filter((r) => fs.every((f) => f(r) === true));
    }

    const aggNodes = [];
    for (const it of q.items) walk(it.e, (n) => n.k === "agg" && aggNodes.push(n));
    walk(q.having, (n) => n.k === "agg" && aggNodes.push(n));
    for (const o of q.orderBy ?? []) walk(o.e, (n) => n.k === "agg" && aggNodes.push(n));
    aggNodes.forEach((n, i) => (n.slot = i));
    let groups;
    if (q.groupBy || aggNodes.length) {
        // one pass: each group keeps its first row and one accumulator per aggregate
        const keys = (q.groupBy ?? []).map((g) => compile(g, globalSlot)),
            args = aggNodes.map((n) => (n.arg ? compile(n.arg, globalSlot) : null));
        const keyOf =
            keys.length === 0
                ? () => ""
                : keys.length === 1
                  ? (r) => keys[0](r) ?? null
                  : (r) => JSON.stringify(keys.map((k) => k(r) ?? null));
        const byKey = new Map();
        for (const r of rows) {
            const key = keyOf(r);
            let g = byKey.get(key);
            if (!g)
                byKey.set(
                    key,
                    (g = {
                        row: r,
                        acc: aggNodes.map((n) => ({ n: 0, sum: 0, best: null, seen: n.distinct ? new Set() : null })),
                    }),
                );
            for (let i = 0; i < aggNodes.length; i++) {
                const a = g.acc[i],
                    v = args[i] ? args[i](r) : 1;
                if (isNull(v)) continue;
                if (a.seen) {
                    if (a.seen.has(v)) continue;
                    a.seen.add(v);
                }
                a.n++;
                a.sum += v;
                const fn = aggNodes[i].fn;
                if (
                    a.best === null ||
                    (fn === "MIN" ? sqlCompare(v, a.best) < 0 : fn === "MAX" && sqlCompare(v, a.best) > 0)
                )
                    a.best = v;
            }
        }
        if (!q.groupBy && byKey.size === 0)
            byKey.set("", {
                row: new Array(q.width).fill(null),
                acc: aggNodes.map(() => ({ n: 0, sum: 0, best: null })),
            });
        plan.push(`hash aggregate ${byKey.size} groups, ${aggNodes.length} aggregates`);
        groups = byKey
            .values()
            .map(({ row, acc }) => ({
                row,
                aggs: acc.map((a, i) => {
                    const fn = aggNodes[i].fn;
                    if (fn === "COUNT") return a.n;
                    if (!a.n) return null;
                    return fn === "SUM" ? a.sum : fn === "AVG" ? a.sum / a.n : a.best;
                }),
            }))
            .toArray();
        if (q.having) {
            const h = compile(q.having, globalSlot);
            groups = groups.filter((g) => h(g.row, g.aggs) === true);
        }
    } else groups = rows.map((row) => ({ row, aggs: null }));

    const items = q.items.map((it) => (it.star ? null : compile(it.e, globalSlot)));
    const project = (g) => {
        const out = [];
        for (let i = 0; i < items.length; i++) {
            if (!items[i]) for (const v of g.row) out.push(v);
            else {
                const v = items[i](g.row, g.aggs);
                out.push(typeof v === "boolean" ? Number(v) : (v ?? null));
            }
        }
        return out;
    };
    let out = groups.map((g) => ({ g, vals: project(g) }));
    if (q.distinct) out = out.unique((x) => JSON.stringify(x.vals));
    if (q.orderBy) {
        const keyFns = q.orderBy.map((o) => {
            const at = o.output;
            if (at !== undefined) return (x) => x.vals[at];
            const f = compile(o.e, globalSlot);
            return (x) => f(x.g.row, x.g.aggs);
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
        out.forEach((x) => (x.key = keyFns.map((f) => f(x))));
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
db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, customer_id INTEGER, amount REAL, status TEXT, day INTEGER)");
for (const name of ["orders", "sample"])
    for (const t of tuples[name]) db.exec(`INSERT INTO ${name} VALUES (?, ?, ?, ?, ?)`, t);
db.exec("COMMIT");

const norm = (v) => (typeof v === "number" && !Number.isInteger(v) ? Math.round(v * 1e6) / 1e6 : v);
const canon = (rows, ordered) => {
    const r = rows.map((x) => JSON.stringify(x.map(norm)));
    return ordered ? r : r.sort();
};

// an amount whose cents are not exact in binary (0.07 * 100 = 7.000000000000001): an index bound computed from it
// must still include the row holding exactly that amount
const inexact = tables.orders.find((o) => o.amount * 100 > Math.round(o.amount * 100)).amount,
    inexactLow = tables.orders.find((o) => o.amount * 100 < Math.round(o.amount * 100)).amount;
const QUERIES = [
    [
        "an inexact decimal bound keeps its own row",
        `SELECT COUNT(*) AS n FROM orders WHERE amount BETWEEN ${inexact} AND ${inexactLow + 1000} AND amount >= ${inexact}`,
        "index range amount",
    ],
    [
        "an inexact upper bound keeps its own row",
        `SELECT COUNT(*) AS n FROM orders WHERE amount <= ${inexactLow}`,
        "index range amount <=",
    ],
    [
        "NOT BETWEEN with one NULL bound is decided by the other",
        "SELECT COUNT(*) AS n FROM sample WHERE amount NOT BETWEEN 100 AND NULL",
        "seq",
    ],
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
    [
        "LEFT JOIN anti-join finds orphan orders",
        "SELECT COUNT(*) AS n FROM orders o LEFT JOIN customers c ON o.customer_id = c.id WHERE c.id IS NULL",
        "filter(1) after join",
    ],
    [
        "a WHERE on the null-extended side runs above the LEFT JOIN",
        "SELECT COUNT(*) AS n, COUNT(c.id) AS matched FROM sample s LEFT JOIN customers c ON s.customer_id = c.id WHERE c.tier IS NULL OR c.tier = 'gold'",
        "left hash join",
    ],
    [
        "CASE buckets grouped by alias",
        "SELECT CASE WHEN amount < 50 THEN 'small' WHEN amount < 200 THEN 'medium' ELSE 'large' END AS size, COUNT(*) AS n, SUM(amount) AS total FROM orders GROUP BY size ORDER BY size",
        "hash aggregate 3 groups",
    ],
    [
        "COUNT(DISTINCT) per status",
        "SELECT o.status, COUNT(DISTINCT o.customer_id) AS buyers, COUNT(*) AS n FROM orders o GROUP BY o.status ORDER BY o.status",
        "hash aggregate 5 groups",
    ],
    [
        "IN (SELECT ...) runs the subquery once",
        "SELECT COUNT(*) AS n, SUM(amount) AS s FROM orders WHERE customer_id IN (SELECT id FROM customers WHERE tier = 'gold' AND country = 'JP')",
        "seq on orders",
    ],
    [
        "NOT IN a subquery that yields NULL is never true",
        "SELECT COUNT(*) AS n FROM customers WHERE tier NOT IN (SELECT tier FROM customers WHERE country = 'DE')",
        "seq",
    ],
    [
        "NOT IN an empty subquery is true even for NULL",
        "SELECT COUNT(*) AS n FROM customers WHERE tier NOT IN (SELECT tier FROM customers WHERE id < 0)",
        "seq",
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
results[`all ${QUERIES.length} named queries match`] = allEqual;

// =============================================================================
// Random differential: generated queries over the NULL-bearing sample, the customers and the indexed orders
// =============================================================================
// Every operator, NULL literal, LEFT and inner join, GROUP BY with HAVING, aggregates with DISTINCT, IN lists
// and subqueries, CASE, decimal amount bounds that are not exact in binary. Results compare as multisets with a
// relative tolerance on floats (SQLite sums with compensation; the engine sums left to right).
const grng = new Random(4242);
const gpick = (xs) => xs[grng.nextBounded(xs.length)];
const amountLit = () =>
    grng.nextBounded(3) ? String(grng.nextBounded(300)) : (grng.nextBounded(30000) / 100).toFixed(2);
const COLS = {
    num: [
        ["s.amount", amountLit],
        ["s.day", () => String(grng.nextBounded(365))],
        ["s.customer_id", () => String(grng.nextBounded(2200))],
        ["c.id", () => String(grng.nextBounded(2200))],
    ],
    text: [
        ["s.status", () => `'${gpick(STATUSES)}'`],
        ["c.tier", () => `'${gpick(["gold", "silver", "bronze"])}'`],
        ["c.country", () => `'${gpick(COUNTRIES)}'`],
    ],
};
function genAtom() {
    const kind = grng.nextBounded(3) ? "num" : "text",
        [col, lit] = gpick(COLS[kind]),
        litOrNull = () => (grng.nextBounded(12) ? lit() : "NULL"),
        not = grng.nextBounded(4) ? "" : "NOT ";
    switch (grng.nextBounded(kind === "num" ? 7 : 5)) {
        case 0:
            return `${col} ${gpick(["=", "!=", "<", "<=", ">", ">="])} ${litOrNull()}`;
        case 1:
            return `${col} ${not}IN (${Array.from({ length: 1 + grng.nextBounded(3) }, litOrNull).join(", ")})`;
        case 2:
            return `${col} IS ${not}NULL`;
        case 3:
            return kind === "num"
                ? `${col} ${not}BETWEEN ${litOrNull()} AND ${litOrNull()}`
                : `${col} ${not}LIKE '${gpick(["%a%", "s%", "_o%", "%d", "p__d", "%"])}'`;
        case 4:
            return kind === "num"
                ? `${col} + s.day * 2 > ${lit()}`
                : `${col} ${not}IN (SELECT ${gpick(["tier", "country"])} FROM customers WHERE id < ${grng.nextBounded(60)})`;
        case 5:
            return `CASE WHEN ${col} > ${lit()} THEN 1 WHEN ${col} IS NULL THEN NULL ELSE 0 END = 1`;
        default:
            return `s.customer_id ${not}IN (SELECT id FROM customers WHERE tier = '${gpick(["gold", "silver"])}' AND country = '${gpick(COUNTRIES)}')`;
    }
}
function genPred(depth) {
    if (depth > 1 || grng.nextBounded(3) === 0) return genAtom();
    const a = genPred(depth + 1),
        b = genPred(depth + 1);
    return grng.nextBounded(5) === 0 ? `NOT (${a} AND ${b})` : `(${a} ${gpick(["AND", "OR"])} ${b})`;
}
function genQuery() {
    const join = gpick(["JOIN", "LEFT JOIN"]),
        from = `FROM sample s ${join} customers c ON s.customer_id = c.id`;
    const pred = grng.nextBounded(6) ? genPred(0) : null,
        where = pred ? ` WHERE ${pred}` : "";
    // row queries keep to a quarter of the days, so each multiset comparison is a few hundred rows
    if (grng.nextBounded(2))
        return `SELECT s.id AS sid, s.amount, s.status, c.id AS cid, c.tier ${from} WHERE s.day < 90${pred ? ` AND (${pred})` : ""}`;
    const g = gpick(["c.tier", "s.status", "c.country", "s.day / 100"]);
    const aggs = [
        "COUNT(*)",
        "COUNT(c.tier)",
        "SUM(s.amount)",
        "AVG(s.amount)",
        "MIN(s.status)",
        "MAX(s.amount)",
        "COUNT(DISTINCT s.customer_id)",
    ].filter(() => grng.nextBounded(2));
    const having = grng.nextBounded(3) ? "" : ` HAVING COUNT(*) > ${grng.nextBounded(400)}`;
    const list = [g, "COUNT(*)", ...aggs].map((x, i) => `${x} AS a${i}`).join(", ");
    return `SELECT ${list} ${from}${where} GROUP BY ${g}${having}`;
}
function genIndexed() {
    // single-table queries on orders, where the planner turns amount and status predicates into index scans
    const amount = gpick([
        () => `amount BETWEEN ${amountLit()} AND ${amountLit()}`,
        () => `amount ${gpick([">", ">=", "<", "<="])} ${amountLit()}`,
    ])();
    const status = gpick(["", ` AND status = '${gpick(STATUSES)}'`, ` AND status IN ('paid', 'refunded')`]);
    const pred = grng.nextBounded(2) ? amount + status : status.slice(5) || amount;
    return `SELECT id, amount, status FROM orders WHERE ${pred} AND day < 30`;
}
const close = (a, b) =>
    a === b || (typeof a === "number" && typeof b === "number" && Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a)));
const sortKey = (row) => JSON.stringify(row.map((v) => (typeof v === "number" ? Math.round(v * 1e4) / 1e4 : v)));
function sameRows(mine, theirs) {
    if (mine.length !== theirs.length) return false;
    const a = mine.map((r) => [sortKey(r), r]).sort((x, y) => (x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0)),
        b = theirs.map((r) => [sortKey(r), r]).sort((x, y) => (x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0));
    return a.every(([, r], i) => r.length === b[i][1].length && r.every((v, j) => close(v, b[i][1][j])));
}
// SQLite divides integers as integers; the engine's `/` is real division, so the generator's `s.day / 100`
// is rewritten for SQLite only. That is the one dialect difference the generator emits.
const toSqlite = (sql) => sql.replaceAll("s.day / 100", "s.day / 100.0");
const generated = [...Array.from({ length: 140 }, genQuery), ...Array.from({ length: 30 }, genIndexed)];
let genMismatch = 0,
    genMs = 0,
    genSqliteMs = 0,
    genRows = 0;
const genPlans = new Set();
for (const sql of generated) {
    const t0 = performance.now(),
        mine = execute(sql),
        t1 = performance.now(),
        theirs = db.query(toSqlite(sql)).map((r) => Object.values(r));
    genMs += t1 - t0;
    genSqliteMs += performance.now() - t1;
    genRows += mine.rows.length;
    mine.plan.forEach((s) => genPlans.add(s.replace(/[\d.']+|\(build.*|IN \(\d+\)/g, "").trim()));
    if (!sameRows(mine.rows, theirs)) {
        genMismatch++;
        if (genMismatch <= 3) print(`MISMATCH ${sql}\n  mine ${mine.rows.length} rows, sqlite ${theirs.length}`);
    }
}
print(
    `  generated  ${generated.length} queries, ${genRows} rows: ${genMs.toFixed(0)} ms here, ${genSqliteMs.toFixed(0)} ms in SQLite`,
);
results[`${generated.length} generated queries return SQLite's rows`] =
    genMismatch === 0 &&
    ["left hash join", "hash join", "index range amount", "index lookup status", "filter() after join"].every((k) =>
        [...genPlans].some((s) => s.startsWith(k)),
    );

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
