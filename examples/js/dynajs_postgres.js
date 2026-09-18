// dynajs_postgres.js — the dyna:net PostgreSQL client, driven against a mock backend.
//
// dyna:net exposes PostgreSQL: a frontend/backend protocol 3.0 client that runs
// on the JS thread via the shared reactor. Every query returns a Promise, and
// replies are matched to queries by POSITION — one ReadyForQuery ends exactly
// one query, simple or extended.
//
// No postgres server is required. The backend here is a TCPServer speaking the
// real wire protocol with `trust` authentication, which is also the more useful
// thing to read: it shows the exact framing. A typed message is a type byte
// then an Int32 length that INCLUDES ITSELF but not the type byte — getting
// that off by one is the classic PostgreSQL framing bug.
//
// Run:  dynajs examples/js/dynajs_postgres.js
//
// API:
//   const db = new PostgreSQL({ host, port, path, user, password, database,
//                             applicationName, raw, insecureAuth,
//                             maxMessageBytes, maxPending,
//                             connectTimeoutMs, queryTimeoutMs });
//   db.query(sql)          -> Promise<{rows, fields, command, rowCount}>
//   db.query(sql, params)  -> same, via Parse/Bind/Describe/Execute/Sync
//   db.cancel()            -> void, on a FRESH connection
//   db.on("notice"|"notification"|"error", fn) -> db
//   db.ready  db.pending  db.backendPid  db.transactionStatus
//   db.parameters  db.close()
//   An ErrorResponse rejects with an Error whose .code is the SQLSTATE, plus
//   .severity/.detail/.hint/.position. A connection failure rejects with
//   .code === "CONNECTION". TLS is not supported and SCRAM-SHA-256 is the only
//   accepted mechanism unless `insecureAuth` is set.

import { TCPServer, PostgreSQL } from "dyna:net";

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) { print("  FAIL: " + what); failures++; }
}

/* ---- byte helpers -------------------------------------------------------- */

function bytes(s) {
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
  return a;
}
function latin1(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
}
function i32(v) {
  return String.fromCharCode((v >>> 24) & 0xff, (v >>> 16) & 0xff,
                             (v >>> 8) & 0xff, v & 0xff);
}
function i16(v) { return String.fromCharCode((v >>> 8) & 0xff, v & 0xff); }
function rd32(s, at) {
  return ((s.charCodeAt(at) << 24) | (s.charCodeAt(at + 1) << 16) |
          (s.charCodeAt(at + 2) << 8) | s.charCodeAt(at + 3)) >>> 0;
}
function cstr(s) { return s + "\0"; }

/* type byte, then a length that includes itself but not the type byte */
function msg(type, body) { return type + i32(body.length + 4) + body; }

function rowDesc(cols) {
  let b = i16(cols.length);
  for (const c of cols)
    b += cstr(c.name) + i32(0) + i16(0) + i32(c.oid) +
         i16(0xffff) + i32(0xffffffff) + i16(0);
  return msg("T", b);
}
function dataRow(vals) {
  let b = i16(vals.length);
  for (const v of vals)
    b += v === null ? i32(0xffffffff) : i32(v.length) + v;
  return msg("D", b);
}

/* ---- a PostgreSQL backend, in JS ----------------------------------------- */

function makeBackend() {
  const bufs = new Map(), started = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.startup = null;
  srv.queries = [];
  let lastSql = null, lastParams = [];

  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + latin1(b));
      for (;;) {
        const s = bufs.get(c);
        if (!started.get(c)) {
          /* the startup packet has NO type byte: length, then the version */
          if (s.length < 8) return;
          const len = rd32(s, 0);
          if (s.length < len) return;
          bufs.set(c, s.slice(len));
          started.set(c, true);
          srv.startup = { version: rd32(s, 4), body: s.slice(8, len) };
          /* trust auth: AuthenticationOk, parameters, the cancel key, ready */
          c.write(bytes(msg("R", i32(0)) +
                        msg("S", cstr("server_version") + cstr("18.0")) +
                        msg("S", cstr("client_encoding") + cstr("UTF8")) +
                        msg("K", i32(4242) + "abcd") +
                        msg("Z", "I")));
          continue;
        }
        if (s.length < 5) return;
        const type = s[0], len = rd32(s, 1);
        if (s.length < len + 1) return;
        const body = s.slice(5, len + 1);
        bufs.set(c, s.slice(len + 1));

        let out = null;
        if (type === "Q") {                       /* Simple Query */
          const sql = body.slice(0, body.length - 1);
          srv.queries.push({ kind: "simple", sql: sql });
          out = results(sql, []) + msg("Z", "I");
        } else if (type === "P") {                /* Parse */
          const at = body.indexOf("\0") + 1;
          lastSql = body.slice(at, body.indexOf("\0", at));
          out = msg("1", "");
        } else if (type === "B") {                /* Bind */
          let at = body.indexOf("\0") + 1;        /* portal name */
          at = body.indexOf("\0", at) + 1;        /* statement name */
          const nfmt = (body.charCodeAt(at) << 8) | body.charCodeAt(at + 1);
          at += 2 + nfmt * 2;
          const np = (body.charCodeAt(at) << 8) | body.charCodeAt(at + 1);
          at += 2;
          lastParams = [];
          for (let i = 0; i < np; i++) {
            const l = rd32(body, at);
            at += 4;
            if (l === 0xffffffff) { lastParams.push(null); continue; }
            lastParams.push(body.slice(at, at + l));
            at += l;
          }
          srv.queries.push({ kind: "extended", sql: lastSql, params: lastParams });
          out = msg("2", "");
        } else if (type === "D") {                /* Describe the portal */
          out = msg("n", "");
        } else if (type === "E") {                /* Execute */
          out = results(lastSql, lastParams);
        } else if (type === "S") {                /* Sync */
          out = msg("Z", "I");
        }
        if (out) c.write(bytes(out));
      }
    },
    close: (c) => { bufs.delete(c); started.delete(c); },
  });
  return srv;
}

/* The reply table. Everything this example prints comes from here. */
function results(sql, params) {
  if (/^SELECT typed/.test(sql))
    return rowDesc([{ name: "flag",  oid: 16   },   /* bool      */
                    { name: "n",     oid: 23   },   /* int4      */
                    { name: "big",   oid: 20   },   /* int8      */
                    { name: "huge",  oid: 20   },   /* int8      */
                    { name: "ratio", oid: 701  },   /* float8    */
                    { name: "money", oid: 1700 },   /* numeric   */
                    { name: "when",  oid: 1114 },   /* timestamp */
                    { name: "gone",  oid: 25   }])  /* text      */ +
           dataRow(["t", "42", "9007199254740992", "9007199254740993",
                    "1.5", "0.10", "2026-07-30 16:47:00", null]) +
           msg("C", cstr("SELECT 1"));
  if (/^SELECT echo/.test(sql))
    return rowDesc([{ name: "given", oid: 25 }]) +
           dataRow([params.length ? params[0] : null]) +
           msg("C", cstr("SELECT 1"));
  if (/^SELECT nope/.test(sql))
    return msg("E", "S" + cstr("ERROR") + "V" + cstr("ERROR") +
                    "C" + cstr("42P01") +
                    "M" + cstr('relation "nope" does not exist') +
                    "P" + cstr("15") +
                    "H" + cstr("check the search_path") + "\0");
  if (/^INSERT/.test(sql))
    return msg("C", cstr("INSERT 0 3"));
  return rowDesc([{ name: "one", oid: 23 }]) + dataRow(["1"]) +
         msg("C", cstr("SELECT 1"));
}

/* ---- drive it ------------------------------------------------------------ */

const srv = makeBackend();

async function main() {
  const db = new PostgreSQL({ host: "127.0.0.1", port: srv.port,
                            user: "app", database: "shop",
                            applicationName: "dynajs-example" });

  // 1. a simple query. The startup packet, the authentication exchange and
  //    ReadyForQuery already happened; a query issued before that is HELD
  //    rather than written into the middle of the handshake.
  const one = await db.query("SELECT 1");
  print("simple    -> rows:", JSON.stringify(one.rows),
        " command:", JSON.stringify(one.command),
        " rowCount:", one.rowCount);
  print("fields    ->", JSON.stringify(one.fields));
  check(one.rows.length === 1 && one.rows[0].one === 1, "SELECT 1");
  check(one.command === "SELECT 1", "the command tag");
  check(one.fields[0].name === "one" && one.fields[0].typeOid === 23,
        "the field descriptor carries the name and the type OID");

  // 2. what the startup packet actually asked for.
  print("startup   -> protocol", srv.startup.version, "(3.0 is 196608)");
  print("           keys:",
        JSON.stringify(srv.startup.body.split("\0").filter((x) => x)));
  print("session   -> backendPid:", db.backendPid,
        " transactionStatus:", JSON.stringify(db.transactionStatus),
        " ready:", db.ready);
  print("parameters->", JSON.stringify(db.parameters));
  check(srv.startup.version === 196608, "protocol 3.0 is requested");
  check(db.backendPid === 4242, "BackendKeyData's pid is kept");
  check(db.transactionStatus === "I", "ReadyForQuery's status is surfaced");
  check(db.parameters.server_version === "18.0", "ParameterStatus is collected");

  // 3. the type mapping, and where it deliberately stops.
  const t = (await db.query("SELECT typed")).rows[0];
  print("bool      ->", JSON.stringify(t.flag),  typeof t.flag);
  print("int4      ->", JSON.stringify(t.n),     typeof t.n);
  print("int8 @2^53->", JSON.stringify(t.big),   typeof t.big);
  print("int8 >2^53->", JSON.stringify(t.huge),  typeof t.huge,
        "(text: a double would round it)");
  print("float8    ->", JSON.stringify(t.ratio), typeof t.ratio);
  print("numeric   ->", JSON.stringify(t.money), typeof t.money,
        "(text: 0.10 is not 0.1)");
  print("timestamp ->", JSON.stringify(t.when),  typeof t.when,
        "(text: exact, offset and all)");
  print("NULL      ->", JSON.stringify(t.gone),  '(null, not "")');
  check(t.flag === true, "bool 't' becomes true");
  check(t.n === 42, "int4 becomes a number");
  check(t.big === 9007199254740992, "an int8 at 2^53 is still a number");
  check(t.huge === "9007199254740993", "an int8 past 2^53 stays text");
  check(t.ratio === 1.5, "float8 becomes a number");
  check(t.money === "0.10", "numeric stays text");
  check(t.when === "2026-07-30 16:47:00", "a timestamp stays text");
  check(t.gone === null, "a -1 column length is NULL");

  // 4. a parameterised query goes through Parse/Bind/Describe/Execute/Sync, so
  //    the parameter is a VALUE — length-prefixed, in its own message, never
  //    text spliced into the statement. The placeholders are $1, $2 (not ?).
  const nasty = "'; DROP TABLE users; --";
  const echoed = await db.query("SELECT echo $1", [nasty]);
  const ext = srv.queries.filter((q) => q.kind === "extended");
  print("param     -> sent:", JSON.stringify(ext[0].params));
  print("           got back:", JSON.stringify(echoed.rows[0].given));
  print("           the SQL the server parsed:", JSON.stringify(ext[0].sql));
  check(ext.length === 1, "a parameterised query uses Parse/Bind");
  check(ext[0].params[0] === nasty, "the parameter arrives separate from the SQL");
  check(ext[0].sql === "SELECT echo $1", "and the statement is untouched");
  check(echoed.rows[0].given === nasty, "and it round-trips verbatim");

  // 5. null is SQL NULL (a length of -1), not the empty string. And an object
  //    is REFUSED: stringifying one stores "[object Object]" and reports
  //    success, which is the quietest way to lose data there is.
  await db.query("SELECT echo $1", [null]);
  const withNull = srv.queries.filter((q) => q.kind === "extended")[1];
  print("null param->", JSON.stringify(withNull.params));
  check(withNull.params[0] === null, "null is -1, not a zero-length value");

  let objErr = null;
  try { await db.query("SELECT $1", [{ user: 7 }]); }
  catch (e) { objErr = e.message; }
  print("object    ->", JSON.stringify(objErr));
  check(objErr !== null && /is an object/.test(objErr),
        "an object parameter is refused rather than stringified");

  // 6. a command with no rows still carries its tag.
  const ins = await db.query("INSERT INTO t VALUES (1),(2),(3)");
  print("insert    -> command:", JSON.stringify(ins.command),
        " rows:", ins.rows.length);
  check(ins.command === "INSERT 0 3", "the INSERT tag");
  check(ins.rows.length === 0, "and no rows");

  // 7. an ErrorResponse rejects with its SQLSTATE. It is held until
  //    ReadyForQuery — settling early would leave the messages after it for
  //    the NEXT query to misread — so the query that follows still works.
  try {
    await db.query("SELECT nope");
    check(false, "an ErrorResponse must reject");
  } catch (e) {
    print("error     -> code:", JSON.stringify(e.code),
          " severity:", JSON.stringify(e.severity),
          " position:", JSON.stringify(e.position));
    print("           message:", JSON.stringify(e.message));
    print("           hint:", JSON.stringify(e.hint));
    check(e instanceof Error, "it rejects with an Error");
    check(e.code === "42P01", "carrying the SQLSTATE");
    check(e.severity === "ERROR", "the non-localised severity");
    check(e.position === "15", "and the position in the statement");
  }
  const after = await db.query("SELECT 1");
  print("after err ->", JSON.stringify(after.rows));
  check(after.rows[0].one === 1, "the query after an error gets its own answer");

  db.close();

  // 8. TLS is refused by name. SCRAM protects the credential, not the session,
  //    so connecting in plaintext anyway would misrepresent what you have.
  let refused = null;
  try { new PostgreSQL({ port: 5432, tls: true }); }
  catch (e) { refused = e.message; }
  print("tls:true  ->", JSON.stringify(refused));
  check(refused !== null && /TLS/.test(refused), "tls:true is refused by name");
}

main().then(() => {
  srv.close();
  print(failures === 0
    ? "dynajs_postgres: all " + checks + " checks passed"
    : "dynajs_postgres: " + failures + " FAILED of " + checks);
}, (e) => {
  srv.close();
  print("dynajs_postgres: threw:", e && e.message ? e.message : String(e));
});
