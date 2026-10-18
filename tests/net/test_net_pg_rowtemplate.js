// timeout: 180
import { TCPServer, PostgreSQL } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const eq = (a, b, m) => check(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const PG = { host: "127.0.0.1", port: 55432, user: "postgres",
             password: "pw", database: "djtest" };

let live = null;
try {
  live = new PostgreSQL(PG);
  await live.query("SELECT 1");
} catch (e) {
  live = null;
  print("NOTE: no PostgreSQL on " + PG.port + " (" + (e.message || e) +
        ") -- the live half is SKIPPED, the fake-server half still runs");
}

if (live) {
  const t = "dj_rt_" + (Date.now() % 100000);
  try {
    for (const width of [1, 2, 31, 32, 33, 40]) {
      const cols = [];
      for (let i = 0; i < width; i++) cols.push((i === 1 ? '"__proto__"' : '"w ' + i + '"') + " int4");
      await live.query("DROP TABLE IF EXISTS " + t);
      await live.query("CREATE TABLE " + t + " (" + cols.join(",") + ")");
      await live.query("INSERT INTO " + t + " SELECT " +
        cols.map((c, i) => (i % 2 === 0 ? "NULL" : "g")).join(",") +
        " FROM generate_series(1,3) g");
      const r = await live.query("SELECT * FROM " + t);
      const keys0 = Object.keys(r.rows[0]);
      eq(keys0.length, width, "width " + width + ": every column is an own key");
      eq(JSON.stringify(keys0), JSON.stringify(r.rows[0] ? keys0 : []),
         "width " + width + ": key order is stable");
      if (width >= 2) {
        check(keys0.indexOf("__proto__") >= 0,
              "width " + width + ": a column named __proto__ stays an own key");
        eq(r.rows[0].__proto__, r.rows[0]["__proto__"],
           "width " + width + ": __proto__ reads as DATA, not as the prototype");
        check(r.rows[0].__proto__ !== Object.prototype,
              "width " + width + ": __proto__ did not retarget the prototype");
      }
      check(Object.getPrototypeOf(r.rows[0]) === Object.prototype,
            "width " + width + ": the row's prototype is untouched");
      for (const row of r.rows) {
        for (const k of keys0) {
          const d = Object.getOwnPropertyDescriptor(row, k);
          check(d.writable === true && d.enumerable === true && d.configurable === true,
                "width " + width + ": column " + k + " is a plain writable/enumerable/configurable slot");
        }
        for (let i = 0; i < width; i++) {
          const v = row[keys0[i]];
          check(v === null || typeof v === "number",
                "width " + width + ": cell " + i + " is null or a number, got " + typeof v);
        }
      }
    }

    await live.query("DROP TABLE IF EXISTS " + t + "t");
    await live.query("CREATE TABLE " + t + "t (a int4, b text, c float8, d bool," +
                     " e int8, f bytea, g numeric, h timestamptz)");
    await live.query("INSERT INTO " + t + "t VALUES (1,'x',2.5,true,9007199254740993," +
                     "'\\xdeadbeef',1.25,'2020-01-02 03:04:05+00')");
    await live.query("INSERT INTO " + t + "t VALUES (NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL)");
    const rr = (await live.query("SELECT * FROM " + t + "t ORDER BY a NULLS LAST")).rows;
    eq(rr.length, 2, "typed: both rows arrive");
    eq(rr[0].b, "x", "typed: text is text");
    eq(rr[0].c, 2.5, "typed: float8 is a number");
    eq(rr[0].d, true, "typed: bool is a bool");
    eq(rr[0].e, "9007199254740993", "typed: int8 past 2^53 stays exact text");
    eq(rr[0].f, "\\xdeadbeef", "typed: bytea");
    eq(rr[0].g, "1.25", "typed: numeric stays text");
    eq(Object.keys(rr[1]).length, 8, "typed: an all-NULL row still has every key");
    for (const k of Object.keys(rr[1])) eq(rr[1][k], null, "typed: all-NULL cell " + k);

    await live.query("DROP TABLE IF EXISTS " + t + "d");
    await live.query("CREATE TABLE " + t + "d (a int4, b int4)");
    await live.query("INSERT INTO " + t + "d VALUES (1,2)");
    for (const [sql, nfields] of [["SELECT a, a, b FROM " + t + "d", 3],
                                  ["SELECT a AS z, b AS z FROM " + t + "d", 2],
                                  ["SELECT * FROM " + t + "d d1 JOIN " + t + "d d2 ON d1.a = d2.a", 4]]) {
      const r = await live.query(sql);
      const keys = Object.keys(r.rows[0]);
      eq(new Set(keys).size, keys.length,
         "duplicate column names (" + sql + "): no key is listed twice, got " + JSON.stringify(keys));
      eq(r.fields.length, nfields,
         "duplicate column names (" + sql + "): `fields` still describes every column");
    }
    const dz = (await live.query("SELECT a AS z, b AS z FROM " + t + "d")).rows[0];
    eq(dz.z, 2, "a repeated name holds the LAST column's value, as the define path gives");

    await live.query("DROP TABLE IF EXISTS " + t + "u");
    await live.query('CREATE TABLE ' + t + 'u ("constructor" int4, "0" int4, "toString" int4)');
    await live.query("INSERT INTO " + t + 'u VALUES (1,2,3)');
    const u = (await live.query("SELECT * FROM " + t + "u")).rows[0];
    eq(u.constructor, 1, "oddname: constructor is the value");
    eq(u["0"], 2, "oddname: a numeric-looking name is the value");
    eq(u.toString, 3, "oddname: toString is the value");

    let capped = false;
    try { await live.query("SELECT * FROM " + t + "t", { maxRows: 1 }); }
    catch (e) { capped = true; }
    check(capped, "maxRows still refuses a result that exceeds it");
  } catch (e) {
    fails++;
    print("FAIL: live section threw -- " + (e && (e.stack || e.message || e)));
  }
  try { live.close(); } catch (e) {}
}

const enc = new TextEncoder();
function msg(type, body) {
  const len = body.length + 4;
  const out = new Uint8Array(1 + len);
  out[0] = type.charCodeAt(0);
  out[1] = (len >>> 24) & 0xff; out[2] = (len >>> 16) & 0xff;
  out[3] = (len >>> 8) & 0xff;  out[4] = len & 0xff;
  out.set(body, 5);
  return out;
}
const cstr = (s) => enc.encode(s + "\0");
const i32 = (v) => new Uint8Array([(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]);
const i16 = (v) => new Uint8Array([(v >>> 8) & 255, v & 255]);
function cat(parts) {
  let t = 0; for (const p of parts) t += p.length;
  const o = new Uint8Array(t); let at = 0;
  for (const p of parts) { o.set(p, at); at += p.length; }
  return o;
}
const AUTH_OK = msg("R", i32(0));
const READY = msg("Z", new Uint8Array([0x49]));
const BACKEND_KEY = msg("K", cat([i32(4242), new Uint8Array([1, 2, 3, 4])]));
function rowDesc(names, typeOid) {
  const parts = [i16(names.length)];
  for (const nm of names) parts.push(cstr(nm), i32(0), i16(0), i32(typeOid || 23), i16(-1), i32(-1), i16(0));
  return msg("T", cat(parts));
}
function dataRow(vals) {
  const parts = [i16(vals.length)];
  for (const v of vals) {
    if (v === null) parts.push(i32(0xffffffff));
    else { const b = enc.encode(String(v)); parts.push(i32(b.length), b); }
  }
  return msg("D", cat(parts));
}
const COMPLETE = (t) => msg("C", cstr(t));

function scenario(mode) {
  return new Promise((resolve) => {
    let done = false, client = null, phase = 0;
    const srv = new TCPServer({ port: 0 });
    srv.start({
      data(conn) {
        if (phase === 0) { phase = 1; conn.write(cat([AUTH_OK, BACKEND_KEY, READY])); return; }
        if (phase === 1) { phase = 2; try { mode(conn); } catch (e) { print("  srv " + e); } }
      }
    });
    const fin = (tag, v) => {
      if (done) return; done = true;
      try { client && client.close(); } catch (e) {}
      try { srv.close(); } catch (e) {}
      resolve([tag, v]);
    };
    client = new PostgreSQL({ host: "127.0.0.1", port: srv.port,
                              user: "u", password: "p", database: "d",
                              connectTimeoutMs: 4000, queryTimeoutMs: 9000 });
    client.query("SELECT 1").then((v) => fin("value", v), (e) => fin("error", e));
    setTimeout(() => fin("timeout", null), 1500);
  });
}

{
  const [tag, v] = await scenario((c) => c.write(cat([
    rowDesc(["a", "b"], 23), dataRow([1, 2]), COMPLETE("SELECT 1"), READY])));
  check(tag === "value" && JSON.stringify(v.rows[0]) === '{"a":1,"b":2}',
        "control: a DataRow that agrees with its RowDescription decodes, got " +
        tag + " " + JSON.stringify(v && v.rows));
}

{
  const [tag, v] = await scenario((c) => c.write(cat([
    rowDesc(["a", "b", "d"], 23), dataRow([1]), COMPLETE("SELECT 1"), READY])));
  if (tag !== "value") check(false, "short DataRow: expected a row, got " + tag + " " + v);
  else {
    const r = v.rows[0];
    eq(JSON.stringify(Object.keys(r)), '["a"]',
       "short DataRow: only the delivered column is an own key");
    check(!Object.prototype.hasOwnProperty.call(r, "b"),
          "short DataRow: an undelivered column is ABSENT, not present-and-undefined");
    eq(r.a, 1, "short DataRow: the delivered value is right");
    eq(v.fields.length, 3, "short DataRow: `fields` still describes all three");
  }
}

{
  const [tag, v] = await scenario((c) => c.write(cat([
    rowDesc(["a", "b"], 23), dataRow([1, null]), COMPLETE("SELECT 1"), READY])));
  check(tag === "value" && JSON.stringify(v.rows[0]) === '{"a":1,"b":null}',
        "a NULL cell inside a full-width DataRow is null, got " +
        tag + " " + JSON.stringify(v && v.rows));
}

{
  const [tag, v] = await scenario((c) => c.write(cat([
    rowDesc(["a"], 23), dataRow([1, 2, 3]), COMPLETE("SELECT 1"), READY])));
  check(tag === "value" && JSON.stringify(v.rows[0]) === '{"a":1}',
        "long DataRow: the extra cells are dropped, got " +
        tag + " " + JSON.stringify(v && v.rows));
}

{
  const [tag, v] = await scenario((c) => c.write(cat([
    rowDesc(["a", "b"], 23), dataRow([1, 2]), dataRow([3]), COMPLETE("SELECT 1"), READY])));
  check(tag === "value" && v.rows.length === 2
        && JSON.stringify(v.rows[0]) === '{"a":1,"b":2}'
        && JSON.stringify(Object.keys(v.rows[1])) === '["a"]',
        "mixed-arity rows in one result each get their own shape, got " +
        tag + " " + JSON.stringify(v && v.rows));
}

{
  const [tag, v] = await scenario((c) => c.write(cat([
    rowDesc(["a", "b"], 23),
    msg("D", cat([i16(2), i32(1), enc.encode("1")])),
    COMPLETE("SELECT 1"), READY])));
  check(tag === "error" && /DataRow/.test(String(v && v.message)),
        "a truncated DataRow is refused and names the DataRow, got " + tag + " " + v);
}

if (fails === 0) print("test_net_pg_rowtemplate: all " + n + " checks passed");
else {
  print("test_net_pg_rowtemplate: " + fails + " FAILED of " + n);
  throw new Error("test_net_pg_rowtemplate: " + fails + " of " + n + " checks failed");
}
