// flags: --std
// covers: dyna:postgres.PostgreSQL
// Type decoding (2 of 3): json/jsonb as text or JSON.parse; arrays as text or nested arrays whose elements decode like the scalar column (bigint, numeric, dates, json,
// bytes); lower bounds dropped, at most 6 dims; malformed or ragged text stays a string. Text results (simple query: fake server and live) and binary results (prepared
// after one use, live) decode alike. Rule 18: hostile getters, Proxy, NUL; a throwing, GC-running or Proxy numeric constructor inside an element; deep and wide arrays.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_arrays");
import * as std from "std";
import { ensurePostgres, stopStarted } from "../lib/services.js";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const within = (p, ms = 8000) => Promise.race([p, sleep(ms).then(() => { throw new Error("case timed out after " + ms + " ms"); })]);
async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const em = (r) => r && r.err ? String(r.err.message || r.err) : "(resolved)";
const str = (u) => { let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const i16 = (v) => String.fromCharCode((v >>> 8) & 255, v & 255);
const be = (s, o) => (s.charCodeAt(o) << 24 | s.charCodeAt(o + 1) << 16 | s.charCodeAt(o + 2) << 8 | s.charCodeAt(o + 3)) >>> 0;
const msg = (t, b) => t + i32(b.length + 4) + b;
function syncThrow(f) { try { const c = f(); if (c && c.close) c.close(); return null; } catch (e) { return e; } }
class Cap { constructor(t) { this.t = t; } }
const show = (v) => v instanceof Date ? "D" + v.getTime() : v instanceof Cap ? "N" + v.t : typeof v === "bigint" ? "B" + v : v instanceof Uint8Array ? "U" + Array.from(v).join(".")
  : Array.isArray(v) ? "[" + v.map(show).join(",") + "]" : v && typeof v === "object" ? "{" + Object.keys(v).map((k) => k + ":" + show(v[k])).join(",") + "}" : typeof v + ":" + String(v);

function fake(cols) {
  const bufs = new Map(), live = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + str(b));
      for (;;) {
        const s = bufs.get(c);
        if (!live.get(c)) {
          if (s.length < 8) return;
          const len = be(s, 0);
          if (s.length < len) return;
          bufs.set(c, s.slice(len)); live.set(c, 1);
          c.write(bin(msg("R", i32(0)) + msg("K", i32(7) + "abcd") + msg("Z", "I")));
          continue;
        }
        if (s.length < 5) return;
        const len = be(s, 1);
        if (s.length < len + 1) return;
        const t = s[0];
        bufs.set(c, s.slice(len + 1));
        if (t !== "Q") continue;
        const T = i16(cols.length) + cols.map(([oid], i) => "c" + i + "\0" + i32(0) + i16(0) + i32(oid) + i16(-1) + i32(-1) + i16(0)).join("");
        const D = i16(cols.length) + cols.map(([, v]) => v === null ? i32(-1) : i32(v.length) + v).join("");
        try { c.write(bin(msg("T", T) + msg("D", D) + msg("C", "SELECT 1\0") + msg("Z", "I"))); } catch (e) {}
      }
    },
    close: (c) => { bufs.delete(c); live.delete(c); },
  });
  return srv;
}

await __kitS.section("options: values, types, hostile getters, Proxy, NUL", async () => {
  const dead = { host: "127.0.0.1", port: 1, user: "u", connectTimeoutMs: 200 };
  for (const [name, o, re] of [["json 1", { json: 1 }, /json/], ["json 'JSON'", { json: "JSON" }, /json/], ["json 'parse\\0'", { json: "parse\0" }, /json/],
    ["arrays true", { arrays: true }, /arrays/], ["arrays 'arrays'", { arrays: "arrays" }, /arrays/], ["arrays 'array\\0'", { arrays: "array\0" }, /arrays/],
    ["arrays toString throws", { arrays: { toString() { throw new Error("ts boom"); } } }, /ts boom|arrays/]]) {
    const e = syncThrow(() => new PostgreSQL({ ...dead, ...o }));
    check(e && re.test(String(e.message)), name + " throws: " + e);
  }
  const g = syncThrow(() => new PostgreSQL({ ...dead, get arrays() { throw new Error("getter boom"); } }));
  check(g && /getter boom/.test(g.message), "a throwing arrays getter propagates: " + g);
  const p = syncThrow(() => new PostgreSQL(new Proxy({ ...dead }, { get: (t, k) => { if (k === "json") throw new Error("proxy boom"); return t[k]; } })));
  check(p && /proxy boom/.test(p.message), "a throwing Proxy get for json propagates: " + p);
  for (const o of [{ json: "string" }, { json: "parse" }, { arrays: "string" }, { arrays: "array" }, { json: undefined, arrays: undefined }])
    check(!syncThrow(() => new PostgreSQL({ ...dead, ...o })), "accepted: " + JSON.stringify(o));
});

await __kitS.section("fake server: text results", async () => {
  const wide = "{" + Array.from({ length: 300 }, (_, i) => i).join(",") + "}";
  const deep = "{{{{{{1}}}}}}";
  const cols = [[1007, "{1,2,NULL,-4}"], [1009, '{NULL,"NULL",null,"","a \\"b\\" \\\\c",plain," sp "}'], [1007, "{{1,2},{3,4}}"], [1007, "[0:1]={5,6}"],
    [1007, "{}"], [1007, "{{1,2},{3}}"], [1016, "{9007199254740993,1}"], [1231, "{1.50,NaN}"], [1185, '{"2024-01-02 03:04:05+05:30",infinity}'],
    [1000, "{t,f,NULL}"], [199, '{"{\\"a\\":1}","[1,2]",null}'], [3807, '{"{\\"b\\": [true, null]}"}'], [114, '{"x": [1, 2.5, "s"], "y": null}'], [3802, '{"n": 12345678901234567890}'],
    [1007, wide], [1007, deep], [1001, '{"\\\\x00ff"}'], [1187, '{"1 day",NULL}'], [1007, "{1,2"], [1009, "{a}junk"]];
  const want = [[1, 2, null, -4], [null, "NULL", null, "", 'a "b" \\c', "plain", " sp "], [[1, 2], [3, 4]], [5, 6], [], "{{1,2},{3}}", [9007199254740993n, 1n],
    [new Cap("1.50"), NaN], [new Date(Date.UTC(2024, 0, 1, 21, 34, 5)), Infinity], [true, false, null], [{ a: 1 }, [1, 2], null], [{ b: [true, null] }],
    { x: [1, 2.5, "s"], y: null }, { n: 12345678901234567000 }, Array.from({ length: 300 }, (_, i) => i), [[[[[[1]]]]]], [new Uint8Array([0, 255])],
    [{ months: 0, days: 1, microseconds: 0 }, null], "{1,2", "{a}junk"];
  const srv = fake(cols);
  const base = { host: "127.0.0.1", port: srv.port, user: "u", connectTimeoutMs: 2000 };
  {
    const c = new PostgreSQL({ ...base, arrays: "array", json: "parse", numeric: Cap, dates: "date", bigint: true, bytes: true });
    const r = await settle(within(c.query("SELECT")));
    const row = r.ok ? r.ok.rows[0] : {};
    check(!r.err, "decode mode query resolves: " + em(r));
    cols.forEach((col, i) => check(show(row["c" + i]) === show(want[i]), "decode " + col[0] + " " + JSON.stringify(col[1]).slice(0, 60) + ": got " + show(row["c" + i]).slice(0, 120) + " want " + show(want[i]).slice(0, 120)));
    c.close();
  }
  {
    const c = new PostgreSQL(base);
    const r = await settle(within(c.query("SELECT")));
    const row = r.ok ? r.ok.rows[0] : {};
    cols.forEach((col, i) => check(row["c" + i] === col[1], "default mode keeps the text of " + col[0] + " " + JSON.stringify(col[1]).slice(0, 40)));
    c.close();
  }
  {
    const s2 = fake([[114, '{"a": '], [1231, "{1.5}"]]);
    const c = new PostgreSQL({ host: "127.0.0.1", port: s2.port, user: "u", json: "parse" });
    const r = await settle(within(c.query("SELECT")));
    check(r.err && /JSON|unexpected|SyntaxError/i.test(em(r)), "malformed json in parse mode rejects the query: " + em(r));
    c.close(); s2.close();
  }
  {
    const s3 = fake([[1231, "{1.50,2.50,3.50}"]]);
    const seen = [];
    const mk = (numeric) => new PostgreSQL({ host: "127.0.0.1", port: s3.port, user: "u", arrays: "array", numeric });
    const c1 = mk(class { constructor(t) { throw new Error("elem boom " + t); } });
    const c2 = mk(class { constructor(t) { std.gc(); this.t = t; seen.push(t); } });
    const c3 = mk(new Proxy(Cap, { construct(T, a) { seen.push("p" + a[0]); return new T(a[0]); } }));
    const r1 = await settle(within(c1.query("SELECT"))), r1b = await settle(within(c1.query("SELECT")));
    const r2 = await settle(within(c2.query("SELECT"))), r3 = await settle(within(c3.query("SELECT")));
    check(r1.err && /elem boom 1\.50/.test(em(r1)) && r1b.err, "a throwing constructor inside an element rejects the query, the connection keeps working: " + em(r1));
    check(!r2.err && show(r2.ok.rows[0].c0) === "[{t:string:1.50},{t:string:2.50},{t:string:3.50}]" && !r3.err && seen.join() === "1.50,2.50,3.50,p1.50,p2.50,p3.50",
      "GC-running and Proxy constructors see each element once: " + seen.join() + em(r2) + em(r3));
    c1.close(); c2.close(); c3.close(); s3.close();
  }
  srv.close();
});

const svc = __kitS.listing ? null : await ensurePostgres();
await __kitS.section("live PostgreSQL: text and binary array results decode alike", async () => {
  if (!svc || !svc.ok) { print("test_postgres_postgre_sql_arrays: SKIP live part: " + (svc ? svc.why : "listing")); return; }
  const c = new PostgreSQL({ ...svc.conn, connectTimeoutMs: 5000, arrays: "array", json: "parse", numeric: Cap, dates: "date", bigint: true, bytes: true, prepareAfter: 1 });
  try {
    await c.query("SET TimeZone = 'Asia/Kolkata'");
    const q = "SELECT ARRAY[1,NULL,-3]::int4[] AS i4, ARRAY[[1,2],[3,4]]::int2[] AS i2, ARRAY[9007199254740993,-1]::int8[] AS i8, ARRAY['NaN','Infinity',1.5]::float8[] AS f8,"
      + " ARRAY[true,false,NULL] AS b, ARRAY['a \"q\" \\\\ b','NULL',NULL,'','{x}']::text[] AS t, ARRAY[1.50,-0.001]::numeric[] AS n,"
      + " ARRAY['2024-02-29 12:00:00+00','infinity']::timestamptz[] AS tz, ARRAY['2024-02-29']::date[] AS d, ARRAY['1 day 02:00:00']::interval[] AS iv,"
      + " ARRAY['a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11']::uuid[] AS u, ARRAY['\\x00ff'::bytea] AS by, ARRAY['{\"a\":1}','[2]']::json[] AS js, ARRAY['{\"b\":true}']::jsonb[] AS jb,"
      + " '{\"k\":[1,{\"z\":null}]}'::jsonb AS j1, '[1,\"two\"]'::json AS j2, '{}'::int4[] AS e, '[0:1]={7,8}'::int4[] AS lb, ARRAY[[[1]],[[2]]]::int4[] AS d3,"
      + " (SELECT array_agg(i) FROM generate_series(1, 500) i) AS wide WHERE $1::int = 1";
    const runs = []; for (let k = 0; k < 3; k++) runs.push((await c.query(q, [1])).rows[0]);
    const line = (r) => Object.keys(r).map((k) => k + "=" + show(r[k])).join(" ");
    check(runs.every((r) => line(r) === line(runs[0])), "runs 1-3 (text, then binary) decode alike:\n" + runs.map(line).join("\n").slice(0, 4000));
    const r = runs[2];
    const w = { i4: [1, null, -3], i2: [[1, 2], [3, 4]], i8: [9007199254740993n, -1n], f8: [NaN, Infinity, 1.5], b: [true, false, null], t: ['a "q" \\\\ b', "NULL", null, "", "{x}"],
      n: [new Cap("1.50"), new Cap("-0.001")], tz: [new Date(Date.UTC(2024, 1, 29, 12)), Infinity], d: [new Date(Date.UTC(2024, 1, 29))], iv: [{ months: 0, days: 1, microseconds: 7200000000 }],
      u: ["a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11"], by: [new Uint8Array([0, 255])], js: [{ a: 1 }, [2]], jb: [{ b: true }], j1: { k: [1, { z: null }] }, j2: [1, "two"], e: [], lb: [7, 8],
      d3: [[[1]], [[2]]], wide: Array.from({ length: 500 }, (_, i) => i + 1) };
    for (const k of Object.keys(w)) check(show(r[k]) === show(w[k]), k + ": got " + show(r[k]).slice(0, 160) + " want " + show(w[k]).slice(0, 160));
  } finally { c.close(); }
});
stopStarted();
print(fails ? "test_postgres_postgre_sql_arrays: " + fails + " of " + n + " checks FAILED" : "test_postgres_postgre_sql_arrays: all " + n + " checks passed");
if (fails) std.exit(1);
