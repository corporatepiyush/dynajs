/*
 * Per-column BINARY result format -- decode differential and float exactness.
 *
 * Needs a live server: postgres on 127.0.0.1:55432. Skips LOUDLY otherwise,
 * because a suite whose case count varies between runs is silently skipping.
 *
 * THE ORACLE for binary decoding: the same query, same server, same rows, run
 * with binary requested and with textResults forced -- every value must be
 * identical. Asserting only "it returned something" would pass a decoder that
 * byte-swaps, mis-signs or reads a float as an int. */
import { PostgreSQL } from "dyna:net";
const CFG = { host:"127.0.0.1", port:55432, user:"postgres", password:"pw", database:"djtest" };

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

(async () => {
  {
    const probe = new PostgreSQL(CFG);
    try { await probe.query("SELECT 1"); }
    catch (e) {
      print("test_net_pg_binary: SKIP -- no server on 127.0.0.1:55432");
      try { probe.close(); } catch (e2) {}
      return;
    }
    probe.close();
  }
  const bin = new PostgreSQL(CFG);
  const txt = new PostgreSQL(Object.assign({}, CFG, { textResults: true }));
  let n = 0, fails = 0;
  const ck = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
  try {
    let rb = null, rt = null;
    /* Run enough times to promote the statement AND learn its OIDs, so the
       binary arm is actually engaged; the first two executions are text. */
    for (let i = 0; i < 4; i++) { rb = await bin.query(SQL, ["7"]); rt = await txt.query(SQL, ["7"]); }

    ck(bin.statementCache.preparedHits > 0, "the binary client prepared the statement");
    ck(txt.statementCache.preparedHits > 0, "the text client prepared it too (control)");

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
    /* spot-check the values themselves, not just that the arms agree */
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

    /* bytes:true must give the same bytes through the binary path */
    const bb = new PostgreSQL(Object.assign({}, CFG, { bytes: true }));
    let rr = null;
    for (let i = 0; i < 4; i++) rr = await bb.query("SELECT '\\x0068695cff'::bytea AS b", []);
    const u = rr.rows[0].b;
    ck(u instanceof Uint8Array && u.length === 5 && u[0] === 0 && u[2] === 0x69 && u[4] === 0xff,
       "bytes:true through binary gives the exact bytes, got " +
       Object.prototype.toString.call(u) + " " + (u.length ? Array.from(u) : ""));
    bb.close();
    /* ---- float8 TEXT decode must be EXACT ----
       Found by diffing the text arm against binary: a digit accumulator
       (v = v*10 + d, then scale) cannot round-trip a double. It returned
       -1.0000000000000002e+300 for -1e300 and 1.7976931348623145e+308 for
       DBL_MAX, and handed NaN/Infinity back as STRINGS -- so a column's JS
       type depended on its value. textResults forces the text decoder. */
    {
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
    }

    print(fails === 0 ? "test_net_pg_binary: all " + n + " checks passed"
                      : "test_net_pg_binary: " + fails + " FAILED of " + n);
  } catch (e) { print("ERR " + e + (e.stack ? "\n" + e.stack : "")); }
  finally { bin.close(); txt.close(); }
})();
