// flags: --std
// covers: dyna:postgres.PostgreSQL
import { PostgreSQL } from "dyna:postgres";
import { suite } from "../lib/kit.js";
import { ensurePostgres, stopStarted, PG_PORT, PG_USER, PG_PASSWORD } from "../lib/services.js";
const CFG = { host: "127.0.0.1", port: PG_PORT, user: PG_USER, password: PG_PASSWORD, database: "djtest" };

const SQL =
  "SELECT $1::int4 AS i4, (-$1::int4) AS ineg, $1::int2 AS i2, " +
  "$1::int8 * 1000000000 AS i8, 9223372036854775807::int8 AS i8max, " +
  "(-9223372036854775808)::int8 AS i8min, " +
  "1.5::float4 AS f4, (-0.125)::float4 AS f4neg, " +
  "3.141592653589793::float8 AS f8, (-1e300)::float8 AS f8neg, " +
  "'NaN'::float8 AS fnan, 'Infinity'::float8 AS finf, " +
  "true AS bt, false AS bf, " +
  "'\\x0068695cff'::bytea AS by, ''::bytea AS byempty, " +
  "'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid AS uu, " +
  "'plain text'::text AS tx, 0.10::numeric AS nm, NULL::int4 AS nul, " +
  "'{\"k\":1}'::jsonb AS js";

// The binary and text clients connect, and the comparison query runs four
// times on each, in the kit's before hook (never under --list); with no server
// from tests/lib/services.js ensurePostgres every case is a SKIP.
const t = suite("test_net_pg_binary");
let A = null;
const ck = (c, m) => A.ok(c, m);
let why = null, bin = null, txt = null, rb = null, rt = null;
let svcOk = false;
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why && svcOk) a.must(false, "the services.js PostgreSQL is up but " + why); if (why) a.skip(why); return fn(a); }, o);
t.before(async () => {
  const svc = await ensurePostgres({ database: "djtest" });
  if (!svc.ok) { why = "no PostgreSQL: " + svc.why; return; }
  svcOk = true;
  const probe = new PostgreSQL(CFG);
  try { await probe.query("SELECT 1"); }
  catch (e) { why = "no server on 127.0.0.1:" + CFG.port; }
  try { probe.close(); } catch (e2) {}
  if (why) return;
  bin = new PostgreSQL(CFG);
  txt = new PostgreSQL(Object.assign({}, CFG, { textResults: true }));
  for (let i = 0; i < 4; i++) { rb = await bin.query(SQL, ["7"]); rt = await txt.query(SQL, ["7"]); }
});
t.after(async () => { if (bin) bin.close(); if (txt) txt.close(); await stopStarted(); });

test("both clients prepare the statement", () => {
  ck(bin.statementCache.preparedHits > 0, "the binary client prepared the statement");
  ck(txt.statementCache.preparedHits > 0, "the text client prepared it too (control)");
});

test("every column has the same value and JS type in binary and text", () => {
  const b = rb.rows[0], t = rt.rows[0];
  for (const k of Object.keys(t)) {
    const bv = b[k], tv = t[k];
    const same = (typeof bv === "number" && typeof tv === "number" &&
                  Number.isNaN(bv) && Number.isNaN(tv)) ||
                 String(bv) === String(tv);
    ck(same, "column " + k + ": binary gave " + JSON.stringify(bv) +
             " (" + typeof bv + "), text gave " + JSON.stringify(tv) +
             " (" + typeof tv + ")");
    ck(typeof bv === typeof tv,
       "column " + k + " must keep its JS TYPE across formats: " +
       typeof bv + " vs " + typeof tv);
  }
});

test("binary decoding of each type is exact", () => {
  const b = rb.rows[0];
  ck(b.i4 === 7 && b.ineg === -7, "int4 sign, got " + b.i4 + "/" + b.ineg);
  ck(b.i8max === "9223372036854775807", "int8 max exact, got " + b.i8max);
  ck(b.i8min === "-9223372036854775808", "int8 min exact, got " + b.i8min);
  ck(b.f4 === 1.5 && b.f4neg === -0.125, "float4, got " + b.f4 + "/" + b.f4neg);
  ck(Math.abs(b.f8 - Math.PI) < 1e-15, "float8 pi, got " + b.f8);
  ck(b.f8neg === -1e300, "float8 large negative, got " + b.f8neg);
  ck(Number.isNaN(b.fnan), "float8 NaN, got " + b.fnan);
  ck(b.finf === Infinity, "float8 Infinity, got " + b.finf);
  ck(b.bt === true && b.bf === false, "bool");
  ck(b.by === "\\x0068695cff", "bytea hex form, got " + b.by);
  ck(b.byempty === "\\x", "empty bytea, got " + b.byempty);
  ck(b.uu === "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11", "uuid, got " + b.uu);
  ck(b.nul === null, "NULL stays null, got " + b.nul);
  ck(b.tx === "plain text", "text unaffected");
  ck(b.nm === "0.10", "numeric stays EXACT text, got " + b.nm);
});

test("bytes:true through binary gives the exact bytes", async () => {
  const bb = new PostgreSQL(Object.assign({}, CFG, { bytes: true }));
  let rr = null;
  for (let i = 0; i < 4; i++) rr = await bb.query("SELECT '\\x0068695cff'::bytea AS b", []);
  const u = rr.rows[0].b;
  ck(u instanceof Uint8Array && u.length === 5 && u[0] === 0 && u[2] === 0x69 && u[4] === 0xff,
     "bytes:true through binary gives the exact bytes, got " +
     Object.prototype.toString.call(u) + " " + (u.length ? Array.from(u) : ""));
  bb.close();
});

test("float8 text decoding is exact and special values are numbers", async () => {
  const CASES = [
    ["-1e300", -1e300], ["1e300", 1e300], ["1e-300", 1e-300],
    ["2.2250738585072014e-308", 2.2250738585072014e-308],
    ["1.7976931348623157e308", 1.7976931348623157e308],
    ["5e-324", 5e-324], ["0.1", 0.1], ["-0.0", -0.0],
    ["3.141592653589793", 3.141592653589793],
    ["9007199254740993", 9007199254740992],
    ["1234567890123456789", 1234567890123456768],
  ];
  for (const [lit, want] of CASES) {
    const got = (await txt.query("SELECT '" + lit + "'::float8 AS v", [])).rows[0].v;
    ck(Object.is(got, want),
       "float8 text decode of " + lit + " must be EXACT: got " + got);
  }
  for (const [lit, want] of [["NaN", NaN], ["Infinity", Infinity],
                             ["-Infinity", -Infinity]]) {
    const got = (await txt.query("SELECT '" + lit + "'::float8 AS v", [])).rows[0].v;
    ck(typeof got === "number" &&
       (Number.isNaN(want) ? Number.isNaN(got) : got === want),
       "float8 " + lit + " must be a NUMBER, not a string: got " +
       JSON.stringify(got) + " (" + typeof got + ")");
  }
});

await t.run();
