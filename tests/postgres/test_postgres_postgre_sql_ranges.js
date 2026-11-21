// flags: --std
// covers: dyna:postgres.PostgreSQL
// Type decoding (3 of 3): ranges as text or { empty, lower, upper, lowerInclusive, upperInclusive } (unbounded side null, bounds decode like the subtype: bigint,
// numeric, dates); multiranges as arrays of those; malformed text stays a string. Text results (fake server and live) and binary results (prepared after one use, live)
// decode alike. Rule 18: hostile getters, Proxy, NUL; a throwing, GC-running or Proxy numeric constructor inside a bound.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_ranges");
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

const R = (lower, upper, li, ui) => ({ empty: false, lower, upper, lowerInclusive: li, upperInclusive: ui });
const E = { empty: true, lower: null, upper: null, lowerInclusive: false, upperInclusive: false };

await __kitS.section("options: values, types, hostile getters, Proxy, NUL", async () => {
  const dead = { host: "127.0.0.1", port: 1, user: "u", connectTimeoutMs: 200 };
  for (const [name, o] of [["ranges 1", { ranges: 1 }], ["ranges 'range'", { ranges: "range" }], ["ranges 'object\\0'", { ranges: "object\0" }],
    ["ranges toString throws", { ranges: { toString() { throw new Error("ts boom"); } } }]]) {
    const e = syncThrow(() => new PostgreSQL({ ...dead, ...o }));
    check(e && /ranges|ts boom/.test(String(e.message)), name + " throws: " + e);
  }
  const g = syncThrow(() => new PostgreSQL({ ...dead, get ranges() { throw new Error("getter boom"); } }));
  check(g && /getter boom/.test(g.message), "a throwing ranges getter propagates: " + g);
  const p = syncThrow(() => new PostgreSQL(new Proxy({ ...dead }, { get: (t, k) => { if (k === "ranges") throw new Error("proxy boom"); return t[k]; } })));
  check(p && /proxy boom/.test(p.message), "a throwing Proxy get for ranges propagates: " + p);
  for (const o of [{ ranges: "string" }, { ranges: "object" }, { ranges: undefined }]) check(!syncThrow(() => new PostgreSQL({ ...dead, ...o })), "accepted: " + JSON.stringify(o));
});

await __kitS.section("fake server: text results", async () => {
  const cols = [[3904, "[1,5)"], [3904, "empty"], [3904, "(,5]"], [3904, "[1,)"], [3926, "[9007199254740993,9007199254740995)"], [3906, "[1.50,2.50]"],
    [3910, '["2024-01-02 03:04:05+05:30",infinity)'], [3912, "[2024-01-01,2024-02-01)"], [3908, '("2024-01-01 00:00:00","2024-01-01 12:00:00"]'],
    [4451, "{[1,3), [5,7)}"], [4451, "{}"], [4532, "{[1.5,2.5)}"], [3905, '{"[1,2)",empty}'], [3904, "[1,5"], [4451, "{[1,3) [5,7)}"], [3904, null]];
  const want = [R(1, 5, true, false), E, R(null, 5, false, true), R(1, null, true, false), R(9007199254740993n, 9007199254740995n, true, false), R(new Cap("1.50"), new Cap("2.50"), true, true),
    R(new Date(Date.UTC(2024, 0, 1, 21, 34, 5)), Infinity, true, false), R(new Date(Date.UTC(2024, 0, 1)), new Date(Date.UTC(2024, 1, 1)), true, false),
    R(new Date(Date.UTC(2024, 0, 1)), new Date(Date.UTC(2024, 0, 1, 12)), false, true), [R(1, 3, true, false), R(5, 7, true, false)], [], [R(new Cap("1.5"), new Cap("2.5"), true, false)],
    [R(1, 2, true, false), E], "[1,5", "{[1,3) [5,7)}", null];
  const srv = fake(cols);
  const base = { host: "127.0.0.1", port: srv.port, user: "u", connectTimeoutMs: 2000 };
  {
    const c = new PostgreSQL({ ...base, ranges: "object", arrays: "array", numeric: Cap, dates: "date", bigint: true });
    const r = await settle(within(c.query("SELECT")));
    const row = r.ok ? r.ok.rows[0] : {};
    check(!r.err, "object mode query resolves: " + em(r));
    cols.forEach((col, i) => check(show(row["c" + i]) === show(want[i]), "decode " + col[0] + " " + JSON.stringify(col[1]) + ": got " + show(row["c" + i]) + " want " + show(want[i])));
    c.close();
  }
  {
    const c = new PostgreSQL(base);
    const r = await settle(within(c.query("SELECT")));
    const row = r.ok ? r.ok.rows[0] : {};
    cols.forEach((col, i) => check(row["c" + i] === col[1], "default mode keeps the text of " + col[0] + " " + JSON.stringify(col[1])));
    c.close();
  }
  {
    const s3 = fake([[3906, "[1.50,2.50)"]]), seen = [];
    const mk = (numeric) => new PostgreSQL({ host: "127.0.0.1", port: s3.port, user: "u", ranges: "object", numeric });
    const c1 = mk(class { constructor(t) { throw new Error("bound boom " + t); } });
    const c2 = mk(class { constructor(t) { std.gc(); this.t = t; seen.push(t); } });
    const c3 = mk(new Proxy(Cap, { construct(T, a) { seen.push("p" + a[0]); return new T(a[0]); } }));
    const r1 = await settle(within(c1.query("SELECT"))), r1b = await settle(within(c1.query("SELECT")));
    const r2 = await settle(within(c2.query("SELECT"))), r3 = await settle(within(c3.query("SELECT")));
    check(r1.err && /bound boom 1\.50/.test(em(r1)) && r1b.err, "a throwing constructor in a bound rejects the query, the connection keeps working: " + em(r1));
    check(!r2.err && r2.ok.rows[0].c0.upper.t === "2.50" && !r3.err && seen.join() === "1.50,2.50,p1.50,p2.50", "GC-running and Proxy constructors see each bound once: " + seen.join() + em(r2) + em(r3));
    c1.close(); c2.close(); c3.close(); s3.close();
  }
  srv.close();
});

const svc = __kitS.listing ? null : await ensurePostgres();
await __kitS.section("live PostgreSQL: text and binary range results decode alike", async () => {
  if (!svc || !svc.ok) { print("test_postgres_postgre_sql_ranges: SKIP live part: " + (svc ? svc.why : "listing")); return; }
  const c = new PostgreSQL({ ...svc.conn, connectTimeoutMs: 5000, ranges: "object", arrays: "array", numeric: Cap, dates: "date", bigint: true, prepareAfter: 1 });
  try {
    await c.query("SET TimeZone = 'Asia/Kolkata'");
    const ver = Number((await c.query("SHOW server_version_num")).rows[0].server_version_num);
    const q = "SELECT int4range(1, 5) AS a, int4range(1, 5, '[]') AS b, 'empty'::int4range AS e, int4range(NULL, 5) AS lo, int4range(1, NULL) AS hi,"
      + " int8range(9007199254740993, 9007199254740999) AS i8, numrange(1.50, 2.50, '(]') AS n, tstzrange('2024-01-02 03:04:05+00', 'infinity') AS tz,"
      + " tsrange('2024-01-01', '2024-01-02') AS ts, daterange('2024-01-01', '2024-02-01') AS d, ARRAY[int4range(1, 2), 'empty']::int4range[] AS arr"
      + (ver >= 140000 ? ", '{[1,3), [5,7)}'::int4multirange AS m, '{}'::int4multirange AS me" : "") + " WHERE $1::int = 1";
    const runs = []; for (let k = 0; k < 3; k++) runs.push((await c.query(q, [1])).rows[0]);
    const line = (r) => Object.keys(r).map((k) => k + "=" + show(r[k])).join(" ");
    check(runs.every((r) => line(r) === line(runs[0])), "runs 1-3 (text, then binary) decode alike:\n" + runs.map(line).join("\n"));
    const w = { a: R(1, 5, true, false), b: R(1, 6, true, false), e: E, lo: R(null, 5, false, false), hi: R(1, null, true, false), i8: R(9007199254740993n, 9007199254740999n, true, false),
      n: R(new Cap("1.50"), new Cap("2.50"), false, true), tz: R(new Date(Date.UTC(2024, 0, 2, 3, 4, 5)), Infinity, true, false), ts: R(new Date(Date.UTC(2024, 0, 1)), new Date(Date.UTC(2024, 0, 2)), true, false),
      d: R(new Date(Date.UTC(2024, 0, 1)), new Date(Date.UTC(2024, 1, 1)), true, false), arr: [R(1, 2, true, false), E], m: [R(1, 3, true, false), R(5, 7, true, false)], me: [] };
    for (const k of Object.keys(runs[2])) check(show(runs[2][k]) === show(w[k]), k + ": got " + show(runs[2][k]) + " want " + show(w[k]));
  } finally { c.close(); }
});
stopStarted();
print(fails ? "test_postgres_postgre_sql_ranges: " + fails + " of " + n + " checks FAILED" : "test_postgres_postgre_sql_ranges: all " + n + " checks passed");
if (fails) std.exit(1);
