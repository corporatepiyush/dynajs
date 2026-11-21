// flags: --std
// covers: dyna:postgres.PostgreSQL
// Type decoding (1 of 3): numeric as text or new C(text) (NaN/Infinity as numbers); date/timestamp/timestamptz as Date (UTC, floored to ms; outside the Date range an ISO string;
// infinity ±Infinity); interval { months, days, microseconds } (bigint past 2^53, infinity ±Infinity). Text results (simple query: fake server and live) and binary results
// (prepared after one use, live) decode to the same values. Rule 18: hostile getters, Proxy, NUL, a throwing, GC-running or Proxy constructor.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_types");
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
const show = (v) => v instanceof Date ? "D" + v.getTime() : v instanceof Cap ? "N" + v.t : typeof v === "bigint" ? "B" + v
  : v && typeof v === "object" ? "{" + Object.keys(v).map((k) => k + ":" + show(v[k])).join(",") + "}" : typeof v + ":" + String(v);
const utc = (y, mo, d, h = 0, mi = 0, s = 0, ms = 0) => { const t = new Date(0); t.setUTCFullYear(y, mo, d); t.setUTCHours(h, mi, s, ms); return t; };

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
  for (const [name, o, re] of [["numeric 5", { numeric: 5 }, /numeric/], ["numeric 'decimal'", { numeric: "decimal" }, /numeric/],
    ["numeric arrow function", { numeric: () => 1 }, /numeric/], ["numeric {}", { numeric: {} }, /numeric/], ["numeric 'string\\0x'", { numeric: "string\0x" }, /numeric/],
    ["dates 'Date'", { dates: "Date" }, /dates/], ["dates 1", { dates: 1 }, /dates/], ["dates 'date\\0'", { dates: "date\0" }, /dates/],
    ["dates toString throws", { dates: { toString() { throw new Error("ts boom"); } } }, /ts boom|dates/]]) {
    const e = syncThrow(() => new PostgreSQL({ ...dead, ...o }));
    check(e && re.test(String(e.message)), name + " throws: " + e);
  }
  const g = syncThrow(() => new PostgreSQL({ ...dead, get numeric() { throw new Error("getter boom"); } }));
  check(g && /getter boom/.test(g.message), "a throwing numeric getter propagates: " + g);
  const p = syncThrow(() => new PostgreSQL(new Proxy({ ...dead }, { get: (t, k) => { if (k === "dates") throw new Error("proxy boom"); return t[k]; } })));
  check(p && /proxy boom/.test(p.message), "a throwing Proxy get for dates propagates: " + p);
  for (const o of [{ numeric: "string" }, { numeric: Cap }, { numeric: new Proxy(Cap, {}) }, { dates: "date" }, { dates: "string" }, { numeric: undefined, dates: undefined }])
    check(!syncThrow(() => new PostgreSQL({ ...dead, ...o })), "accepted: " + Object.keys(o).map((k) => k + "=" + (typeof o[k] === "function" ? "constructor" : o[k])).join(" "));
});

await __kitS.section("fake server: text results", async () => {
  const cols = [[1700, "1.50"], [1700, "NaN"], [1700, "-Infinity"], [1082, "2024-02-29"], [1082, "infinity"], [1082, "0044-03-15 BC"],
    [1114, "2024-01-02 03:04:05.123456"], [1114, "294276-12-31 23:59:59"], [1184, "2024-01-02 03:04:05+05:30"], [1184, "1900-01-01 00:00:00-00:01:15"],
    [1186, "1 year 2 mons -3 days +04:05:06.789"], [1186, "-178956970 years -8 mons"], [1186, "2562047788:00:54.775807"], [1114, "2024-13-01 00:00:00"],
    [1184, null], [1186, "@ 1 hour"], [1186, "-infinity"], [1114, "-infinity"]];
  const want = [new Cap("1.50"), NaN, -Infinity, utc(2024, 1, 29), Infinity, utc(-43, 2, 15), utc(2024, 0, 2, 3, 4, 5, 123), "294276-12-31 23:59:59",
    utc(2024, 0, 1, 21, 34, 5), utc(1900, 0, 1, 0, 1, 15), { months: 14, days: -3, microseconds: 14706789000 }, { months: -2147483648, days: 0, microseconds: 0 },
    { months: 0, days: 0, microseconds: 9223372036854775807n }, "2024-13-01 00:00:00", null, "@ 1 hour", -Infinity, -Infinity];
  const srv = fake(cols);
  const base = { host: "127.0.0.1", port: srv.port, user: "u", connectTimeoutMs: 2000 };
  {
    const c = new PostgreSQL({ ...base, numeric: Cap, dates: "date" });
    const r = await settle(within(c.query("SELECT")));
    const row = r.ok ? r.ok.rows[0] : {};
    check(!r.err, "decode mode query resolves: " + em(r));
    cols.forEach((col, i) => check(show(row["c" + i]) === show(want[i]), "decode " + col[0] + " " + JSON.stringify(col[1]) + ": got " + show(row["c" + i]) + " want " + show(want[i])));
    c.close();
  }
  {
    const c = new PostgreSQL(base);
    const r = await settle(within(c.query("SELECT")));
    const row = r.ok ? r.ok.rows[0] : {};
    cols.forEach((col, i) => check(row["c" + i] === col[1], "default mode keeps the text of " + col[0] + " " + JSON.stringify(col[1]) + ": " + show(row["c" + i])));
    c.close();
  }
  {
    const c = new PostgreSQL({ ...base, numeric: class { constructor(t) { throw new Error("ctor boom " + t); } } });
    const r1 = await settle(within(c.query("SELECT"))), r2 = await settle(within(c.query("SELECT")));
    check(r1.err && /ctor boom 1\.50/.test(em(r1)) && r2.err && /ctor boom/.test(em(r2)), "a throwing constructor rejects the query and the connection keeps working: " + em(r1) + " / " + em(r2));
    c.close();
  }
  {
    const seen = [];
    const c = new PostgreSQL({ ...base, numeric: class { constructor(t) { std.gc(); this.t = t; seen.push(t); } } });
    const c2 = new PostgreSQL({ ...base, numeric: new Proxy(Cap, { construct(T, a) { seen.push("proxy:" + a[0]); return new T(a[0]); } }) });
    const r = await settle(within(c.query("SELECT"))), r2 = await settle(within(c2.query("SELECT")));
    check(!r.err && r.ok.rows[0].c0.t === "1.50" && !r2.err && r2.ok.rows[0].c0 instanceof Cap && seen.join() === "1.50,proxy:1.50",
      "a GC-running and a Proxy constructor get the text once each, NaN/Infinity never reach them: " + seen.join() + " " + em(r) + em(r2));
    c.close(); c2.close();
  }
  srv.close();
});

const svc = __kitS.listing ? null : await ensurePostgres();
await __kitS.section("live PostgreSQL: text and binary results decode alike", async () => {
  if (!svc || !svc.ok) { print("test_postgres_postgre_sql_types: SKIP live part: " + (svc ? svc.why : "listing")); return; }
  const c = new PostgreSQL({ ...svc.conn, connectTimeoutMs: 5000, numeric: Cap, dates: "date", prepareAfter: 1 });
  try {
    await c.query("SET TimeZone = 'Asia/Kolkata'");
    const ver = Number((await c.query("SHOW server_version_num")).rows[0].server_version_num);
    const runs = async (q) => { const out = []; for (let k = 0; k < 3; k++) out.push((await c.query(q, [1])).rows); return out; };
    const same = (name, rs) => check(rs.every((r) => r.map((x) => Object.values(x).map(show).join("|")).join("\n") === rs[0].map((x) => Object.values(x).map(show).join("|")).join("\n")),
      name + ": runs 1-3 (text, then binary) decode alike:\n" + rs.map((r) => r.map((x) => Object.values(x).map(show).join("|")).join(" ; ")).join("\n"));
    const rn = await runs("SELECT v, v::text AS t FROM unnest(ARRAY['0','1.50','-0.0001','123456789012345678901234567890.123456789','NaN','Infinity','-Infinity','0.00000000000000000001','10000','0.000','-99999999.99990000']::numeric[]) v WHERE $1::int = 1");
    same("numeric", rn);
    for (const rows of rn) for (const x of rows) check(/^-?Infinity$|^NaN$/.test(x.t) ? show(x.v) === show(Number(x.t)) : x.v instanceof Cap && x.v.t === x.t, "numeric " + x.t + " decodes to " + show(x.v));
    for (const ty of ["timestamptz", "timestamp", "date"]) {
      const rt = await runs("SELECT v, (extract(epoch FROM v) * 1000)::float8 AS ms FROM unnest(ARRAY['2024-02-29 12:34:56.789+00','1970-01-01 00:00:00+05:30','0044-03-15 12:00:00 BC','1900-01-01','infinity','-infinity']::" + ty + "[]) v WHERE $1::int = 1");
      same(ty, rt);
      for (const rows of rt) for (const x of rows) check(Math.abs(x.ms) === Infinity ? x.v === x.ms : x.v instanceof Date && Math.abs(x.v.getTime() - x.ms) < 1, ty + " " + x.ms + " decodes to " + show(x.v));
    }
    const ro = await runs("SELECT '294276-12-31 23:59:59'::timestamp AS a, '5874897-12-31'::date AS b, '4713-01-01 BC'::date AS c WHERE $1::int = 1");
    same("out of Date range", ro);
    check(ro[2][0].a === "294276-12-31 23:59:59" && ro[2][0].b === "5874897-12-31" && show(ro[2][0].c) === show(utc(-4712, 0, 1)), "out of Date range stays ISO text: " + Object.values(ro[2][0]).map(show).join(" "));
    const ri = await runs("SELECT '1 year 2 mons 3 days 04:05:06.5'::interval AS a, '-1 days +00:00:01'::interval AS b, '0'::interval AS c, '-178956970 years -7 mons -2147483647 days -2562047788:00:54.775807'::interval AS d" + (ver >= 170000 ? ", 'infinity'::interval AS e, '-infinity'::interval AS f" : "") + " WHERE $1::int = 1");
    same("interval", ri);
    const w = { a: { months: 14, days: 3, microseconds: 14706500000 }, b: { months: 0, days: -1, microseconds: 1000000 }, c: { months: 0, days: 0, microseconds: 0 },
      d: { months: -2147483647, days: -2147483647, microseconds: -9223372036854775807n }, e: Infinity, f: -Infinity };
    for (const k of Object.keys(ri[2][0])) check(show(ri[2][0][k]) === show(w[k]), "interval " + k + ": " + show(ri[2][0][k]));
  } finally { c.close(); }
});
stopStarted();
print(fails ? "test_postgres_postgre_sql_types: " + fails + " of " + n + " checks FAILED" : "test_postgres_postgre_sql_types: all " + n + " checks passed");
if (fails) std.exit(1);
