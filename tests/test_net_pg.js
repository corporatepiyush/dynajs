/* test_net_pg.js -- the dyna:net PostgreSQL client against a MOCK backend.
 *
 * Same reasoning as the Redis test: the cases that decide correctness are the
 * ones a real server cannot be made to produce. A length field of 0 (which,
 * unchecked, makes the framing loop rewind rather than advance), a length past
 * the cap, an unknown type byte, and a BackendKeyData whose key is 4 octets on
 * one connection and 32 on another -- reading that at a fixed width is what
 * desynchronises every message after it.
 *
 * Authentication is `trust` here: SCRAM has its own oracles in tests/test_scram.c,
 * against the RFC 7677 vector and against a server that verifies the proof.
 */
import { TCPServer, PostgreSQL } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

/* ---- byte helpers ---------------------------------------------------- */

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

/* A typed message: type byte, then a length that INCLUDES ITSELF but not the
 * type byte. Getting that off by one is the classic PostgreSQL framing bug. */
function msg(type, body) { return type + i32(body.length + 4) + body; }

/* ---- a PostgreSQL backend, in JS ------------------------------------- */

function makeBackend(opts) {
  opts = opts || {};
  const bufs = new Map(), started = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.queries = [];
  srv.startup = null;
  srv.cancels = [];
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + latin1(b));
      for (;;) {
        let s = bufs.get(c);
        if (!started.get(c)) {
          /* the startup packet has NO type byte */
          if (s.length < 4) return;
          const len = rd32(s, 0);
          if (s.length < len) return;
          const pkt = s.slice(0, len);
          bufs.set(c, s.slice(len));
          const code = rd32(pkt, 4);
          if (code === 80877102) {                 /* CancelRequest */
            srv.cancels.push({ pid: rd32(pkt, 8), keyLen: len - 12 });
            c.close();
            return;
          }
          started.set(c, true);
          srv.startup = { version: code, body: pkt.slice(8) };
          if (opts.saslSkipFinal) {
            /* AuthenticationSASL advertising SCRAM-SHA-256, then -- after the
             * client's SASLInitialResponse -- AuthenticationOk with no
             * SASLFinal, so the server never proves it knows the password. */
            c.write(bytes(msg("R", i32(10) + cstr("SCRAM-SHA-256") + "\0")));
            return;
          }
          if (opts.rejectStartup) {
            c.write(bytes(msg("E", "S" + cstr("FATAL") + "C" + cstr("28000") +
                               "M" + cstr("no pg_hba.conf entry") + "\0")));
            return;
          }
          /* AuthenticationOk, a parameter, the cancel key, ReadyForQuery */
          let out = msg("R", i32(0));
          out += msg("S", cstr("server_version") + cstr("18.0"));
          out += msg("S", cstr("__proto__") + cstr("hijack"));
          out += msg("K", i32(4242) + (opts.key32
                     ? "k".repeat(32) : "abcd"));
          out += msg("Z", "I");
          c.write(bytes(out));
          continue;
        }
        if (s.length < 5) return;
        const type = s[0], len = rd32(s, 1);
        if (s.length < len + 1) return;
        const body = s.slice(5, len + 1);
        bufs.set(c, s.slice(len + 1));
        if (type === "X") { srv.terminated = true; return; }
        if (opts.saslSkipFinal && type === "p") {
          c.write(bytes(msg("R", i32(0)) + msg("Z", "I")));
          return;
        }
        const out = handle(type, body, srv, opts);
        if (out) c.write(bytes(out));
      }
    },
    close: (c) => { bufs.delete(c); started.delete(c); },
  });
  return srv;
}

/* Row helpers. All results in TEXT format, which is what the client asks for. */
function rowDesc(cols) {
  let b = i16(cols.length);
  for (const c of cols)
    b += cstr(c.name) + i32(0) + i16(0) + i32(c.oid) + i16(-1 & 0xffff) +
         i32(-1 >>> 0) + i16(0);
  return msg("T", b);
}
function dataRow(vals) {
  let b = i16(vals.length);
  for (const v of vals)
    b += v === null ? i32(0xffffffff) : i32(v.length) + v;
  return msg("D", b);
}

let lastSql = null;
function handle(type, body, srv, opts) {
  if (type === "Q") {
    const sql = body.slice(0, body.length - 1);
    srv.queries.push({ kind: "simple", sql: sql });
    return reply(sql, [], opts);
  }
  if (type === "P") {                       /* Parse */
    let at = body.indexOf("\0") + 1;
    lastSql = body.slice(at, body.indexOf("\0", at));
    return msg("1", "");
  }
  if (type === "B") {                       /* Bind */
    let at = body.indexOf("\0") + 1;        /* portal */
    at = body.indexOf("\0", at) + 1;        /* statement */
    const nfmt = (body.charCodeAt(at) << 8) | body.charCodeAt(at + 1);
    at += 2 + nfmt * 2;
    const np = (body.charCodeAt(at) << 8) | body.charCodeAt(at + 1);
    at += 2;
    const params = [];
    for (let i = 0; i < np; i++) {
      const l = rd32(body, at);
      at += 4;
      if (l === 0xffffffff) { params.push(null); continue; }
      params.push(body.slice(at, at + l));
      at += l;
    }
    srv.queries.push({ kind: "extended", sql: lastSql, params: params });
    srv.lastParams = params;
    return msg("2", "");
  }
  if (type === "D") return msg("n", "");    /* Describe -> NoData by default */
  if (type === "E") return reply(lastSql, srv.lastParams || [], opts, true);
  if (type === "S") return msg("Z", "I");   /* Sync */
  if (type === "X") return null;            /* Terminate */
  return null;
}

function reply(sql, params, opts, extended) {
  const z = extended ? "" : msg("Z", "I");
  if (opts.garbage) return "@" + i32(4) + z;
  if (opts.zeroLen) return "T" + i32(0);
  if (opts.hugeLen) return "D" + i32(0x7ffffff0);
  if (/^TWOSETS/.test(sql))
    return rowDesc([{ name: "a", oid: 23 }]) + dataRow(["1"]) +
           msg("C", cstr("SELECT 1")) +
           rowDesc([{ name: "b", oid: 23 }]) + dataRow(["2"]) +
           msg("C", cstr("SELECT 1")) + z;
  /* Two RowDescriptions and ONE CommandComplete: only the fields guard can
   * catch this. And two CommandCompletes with no second RowDescription (an
   * `INSERT; INSERT`): only the completion counter can. Split so that neither
   * guard is proved by the other -- redundant guards prove nothing. */
  if (/^TWODESC/.test(sql))
    return rowDesc([{ name: "a", oid: 23 }]) + dataRow(["1"]) +
           rowDesc([{ name: "b", oid: 23 }]) + dataRow(["2"]) +
           msg("C", cstr("SELECT 2")) + z;
  if (/^TWOTAGS/.test(sql))
    return msg("C", cstr("INSERT 0 1")) + msg("C", cstr("INSERT 0 1")) + z;
  if (/^SHORTROW/.test(sql))
    return rowDesc([{ name: "a", oid: 25 }, { name: "b", oid: 25 }]) +
           msg("D", i16(2) + i32(1) + "x") +          /* declares 2, sends 1 */
           msg("C", cstr("SELECT 1")) + z;
  if (/^COPYME/.test(sql))
    return msg("H", "\0" + i16(0)) + z;              /* CopyOutResponse */
  if (/^SEVONLY/.test(sql))
    return msg("E", "S" + cstr("FATAL") + "C" + cstr("57P01") +
                    "M" + cstr("terminating connection") + "\0") + z;
  if (/^ERRORME/.test(sql))
    return msg("E", "S" + cstr("ERROR") + "V" + cstr("ERROR") +
                    "C" + cstr("42P01") + "M" + cstr('relation "nope" does not exist') +
                    "P" + cstr("15") + "\0") + z;
  if (/^SELECT typed/.test(sql))
    return rowDesc([{ name: "b", oid: 16 }, { name: "i", oid: 23 },
                    { name: "big", oid: 20 }, { name: "huge", oid: 20 },
                    { name: "f", oid: 701 }, { name: "num", oid: 1700 },
                    { name: "sci", oid: 701 }, { name: "hex", oid: 701 },
                    { name: "nil", oid: 25 }]) +
           dataRow(["t", "42", "9007199254740992", "9007199254740993",
                    "1.5", "0.10", "-1.25E-3", "0x10", null]) +
           msg("C", cstr("SELECT 1")) + z;
  if (/^SELECT proto/.test(sql))
    return rowDesc([{ name: "__proto__", oid: 25 }]) +
           dataRow(["hijack"]) + msg("C", cstr("SELECT 1")) + z;
  if (/^SELECT param/.test(sql))
    return rowDesc([{ name: "p", oid: 25 }]) +
           dataRow([params.length ? String(params[0]) : ""]) +
           msg("C", cstr("SELECT 1")) + z;
  if (/^INSERT/.test(sql))
    return msg("C", cstr("INSERT 0 3")) + z;
  if (/^SELECT empty/.test(sql))
    return rowDesc([{ name: "x", oid: 25 }]) + msg("C", cstr("SELECT 0")) + z;
  return rowDesc([{ name: "one", oid: 23 }]) + dataRow(["1"]) +
         msg("C", cstr("SELECT 1")) + z;
}

/* ---- drive it -------------------------------------------------------- */

const good = makeBackend({});
const good32 = makeBackend({ key32: true });
const reject = makeBackend({ rejectStartup: true });
const garbage = makeBackend({ garbage: true });
const zeroLen = makeBackend({ zeroLen: true });
const hugeLen = makeBackend({ hugeLen: true });
const noFinal = makeBackend({ saslSkipFinal: true });
const twoSets = makeBackend({});
const twoDesc = makeBackend({});
const twoTags = makeBackend({});
const shortRow = makeBackend({});
const copyOut = makeBackend({});
const sevOnly = makeBackend({});

const results = {};
function rec(k, p) {
  results[k] = "pending";
  p.then((v) => { results[k] = { ok: v }; }, (e) => { results[k] = { err: e }; });
}

const D = {};
D.db = new PostgreSQL({ port: good.port, host: "127.0.0.1", user: "u",
                      database: "d" });
rec("simple", D.db.query("SELECT 1"));
rec("typed", D.db.query("SELECT typed"));
rec("proto", D.db.query("SELECT proto"));
rec("insert", D.db.query("INSERT INTO t VALUES (1)"));
rec("empty", D.db.query("SELECT empty"));
rec("err", D.db.query("ERRORME"));
rec("afterErr", D.db.query("SELECT 1"));
rec("param", D.db.query("SELECT param", ["hello"]));
rec("paramNull", D.db.query("SELECT param", [null]));

D.k32 = new PostgreSQL({ port: good32.port, host: "127.0.0.1", user: "u" });
rec("k32", D.k32.query("SELECT 1"));

D.reject = new PostgreSQL({ port: reject.port, host: "127.0.0.1", user: "u" });
rec("reject", D.reject.query("SELECT 1"));

D.garbage = new PostgreSQL({ port: garbage.port, host: "127.0.0.1", user: "u" });
rec("garbage", D.garbage.query("SELECT 1"));
D.zero = new PostgreSQL({ port: zeroLen.port, host: "127.0.0.1", user: "u" });
rec("zero", D.zero.query("SELECT 1"));
D.huge = new PostgreSQL({ port: hugeLen.port, host: "127.0.0.1", user: "u" });
rec("huge", D.huge.query("SELECT 1"));

/* A server that runs SCRAM and then skips the proof must be REFUSED: without
 * the final message it never showed it knows the password. */
D.noFinal = new PostgreSQL({ port: noFinal.port, host: "127.0.0.1", user: "u",
                           password: "pw" });
rec("noFinal", D.noFinal.query("SELECT 1"));

D.two = new PostgreSQL({ port: twoSets.port, host: "127.0.0.1", user: "u" });
rec("twoSets", D.two.query("TWOSETS"));
D.twoDesc = new PostgreSQL({ port: twoDesc.port, host: "127.0.0.1", user: "u" });
rec("twoDesc", D.twoDesc.query("TWODESC"));
D.twoTags = new PostgreSQL({ port: twoTags.port, host: "127.0.0.1", user: "u" });
rec("twoTags", D.twoTags.query("TWOTAGS"));
D.short = new PostgreSQL({ port: shortRow.port, host: "127.0.0.1", user: "u" });
rec("shortRow", D.short.query("SHORTROW"));
D.copy = new PostgreSQL({ port: copyOut.port, host: "127.0.0.1", user: "u" });
rec("copy", D.copy.query("COPYME"));
D.sev = new PostgreSQL({ port: sevOnly.port, host: "127.0.0.1", user: "u" });
rec("sevOnly", D.sev.query("SEVONLY"));

/* an EMPTY parameter array must still take the extended path, where the server
 * refuses multiple statements -- choosing by length is how a caller believes
 * they are parameterised and is not */
rec("emptyParams", D.db.query("SELECT 1", []));

D.dead = new PostgreSQL({ port: 1, host: "127.0.0.1", user: "u" });
rec("dead", D.dead.query("SELECT 1"));

/* the refusals that are decided without a server */
let tlsErr = null, paramErr = null;
try { new PostgreSQL({ port: 5432, tls: true }); } catch (e) { tlsErr = String(e); }
try { D.db.query("SELECT 1", "notanarray"); } catch (e) { paramErr = String(e); }
let objErr = null, arrErr = null, u8Err = null;
try { D.db.query("SELECT $1", [{ a: 1 }]); } catch (e) { objErr = String(e); }
try { D.db.query("SELECT $1", [[1, 2]]); } catch (e) { arrErr = String(e); }
/* a byte view is bytea, and the ORACLE is what the server received: asserting
 * only that the call did not throw would pass on any encoding at all */
rec("bytea", D.db.query("SELECT $1", [new Uint8Array([0x01, 0xff, 0x00])]));
try { D.db.query("SELECT $1", [new Date()]); } catch (e) { u8Err = String(e); }

/* ---- assert ---------------------------------------------------------- */

function settled() {
  for (const k in results) if (results[k] === "pending") return false;
  return true;
}

let spins = 0;
const t = setInterval(() => {
  if (!settled() && spins++ < 800) return;
  clearInterval(t);

  const val = (k) => results[k] && results[k].ok;
  const err = (k) => results[k] && results[k].err;
  const emsg = (k) => err(k) ? String(err(k).message || err(k)) : "";

  /* 1. the startup packet */
  check(good.startup !== null, "the backend must receive a startup packet");
  check(good.startup && good.startup.version === 196608,
        "protocol 3.0 (196608) must be requested, got " +
        (good.startup && good.startup.version));
  check(good.startup && /\0user\0u\0/.test("\0" + good.startup.body),
        "the startup packet must carry user, got " +
        JSON.stringify(good.startup && good.startup.body));
  check(good.startup && /database\0d\0/.test(good.startup.body),
        "and database");
  check(D.db.ready, "the client must reach ready after ReadyForQuery");
  check(D.db.backendPid === 4242,
        "BackendKeyData's pid must be kept, got " + D.db.backendPid);
  check(D.db.transactionStatus === "I",
        "ReadyForQuery's status, got " + D.db.transactionStatus);
  check(D.db.parameters && D.db.parameters.server_version === "18.0",
        "ParameterStatus must be collected, got " +
        JSON.stringify(D.db.parameters));

  /* 2. a simple query */
  const s = val("simple");
  check(s && Array.isArray(s.rows) && s.rows.length === 1 &&
        (s.rows[0] || {}).one === 1,
        "SELECT 1 -> " + JSON.stringify(s && s.rows));
  check(s && s.command === "SELECT 1", "the command tag, got " + (s && s.command));
  check(s && s.rowCount === 1, "rowCount " + (s && s.rowCount));
  check(s && s.fields && s.fields[0] && s.fields[0].name === "one" &&
        s.fields[0].typeOid === 23,
        "the field descriptor, got " + JSON.stringify(s && s.fields));

  /* 3. type conversion, and where it deliberately stops */
  const ty = val("typed");
  const r0 = ty && ty.rows[0];
  check(r0 && r0.b === true, "bool 't' -> true, got " + JSON.stringify(r0 && r0.b));
  check(r0 && r0.i === 42, "int4 -> number, got " + JSON.stringify(r0 && r0.i));
  check(r0 && r0.big === 9007199254740992,
        "an int8 AT 2^53 is still a number, got " + JSON.stringify(r0 && r0.big));
  check(r0 && r0.huge === "9007199254740993",
        "an int8 PAST 2^53 must stay text (a double rounds it), got " +
        JSON.stringify(r0 && r0.huge));
  check(r0 && r0.f === 1.5, "float8 -> number, got " + JSON.stringify(r0 && r0.f));
  /* Parsed without strtod, which reads LC_NUMERIC for the radix character: a
     comma locale would stop at the '.' and silently return the integer part. */
  check(r0 && r0.sci === -0.00125,
        "an exponent parses locale-independently, got " + JSON.stringify(r0 && r0.sci));
  /* The discriminating case. PostgreSQL never sends "0x10" for a float, but
     strtod ACCEPTS it as 16 -- so a column decoded with strtod returns a number
     where this client returns the text it was given. Pins that the parser
     implements the server's grammar rather than C's. */
  check(r0 && r0.hex === "0x10",
        "a value outside PostgreSQL's float grammar stays TEXT rather than " +
        "being reinterpreted by strtod, got " + JSON.stringify(r0 && r0.hex));
  check(r0 && r0.num === "0.10",
        "numeric stays TEXT -- a double is not exact and '0.10' is not '0.1', " +
        "got " + JSON.stringify(r0 && r0.num));
  check(r0 && r0.nil === null,
        "a -1 column length is NULL, not an empty string, got " +
        JSON.stringify(r0 && r0.nil));

  /* 4. a column named __proto__ */
  const pr = val("proto");
  check(pr && (pr.rows[0] || {}).__proto__ === "hijack",
        "a column named __proto__ must be a KEY, not retarget the prototype, " +
        "got " + JSON.stringify(pr && pr.rows[0]));

  /* 5. commands with no rows */
  check(val("insert") && val("insert").command === "INSERT 0 3",
        "INSERT tag, got " + (val("insert") && val("insert").command));
  check(val("insert") && val("insert").rows.length === 0,
        "and no rows");
  check(val("empty") && val("empty").rows.length === 0 &&
        val("empty").fields.length === 1,
        "a SELECT with no rows still carries its field descriptors");

  /* 6. an ErrorResponse rejects with its SQLSTATE, and the NEXT query works --
   *    the error must be held until ReadyForQuery, or the messages after it
   *    are read as part of the following query. */
  check(err("err"), "an ErrorResponse must reject");
  check(err("err") && err("err").code === "42P01",
        "with its SQLSTATE, got " + (err("err") && err("err").code));
  check(err("err") && /does not exist/.test(emsg("err")),
        "and its message, got " + emsg("err"));
  check(err("err") && err("err").severity === "ERROR",
        "and the non-localised severity, got " + (err("err") && err("err").severity));
  check(err("err") && err("err").position === "15",
        "and the position, got " + (err("err") && err("err").position));
  check(val("afterErr") && (val("afterErr").rows[0] || {}).one === 1,
        "the query AFTER an error must get its own answer, got " +
        JSON.stringify(val("afterErr")));

  /* 7. parameters go through the extended protocol as VALUES */
  const ext = good.queries.filter((q) => q.kind === "extended");
  check(ext.length === 4,
        "every query given a parameter ARRAY uses Parse/Bind -- including the " +
        "one given an empty array -- got " + ext.length);
  check(ext[0] && ext[0].params.length === 1 && ext[0].params[0] === "hello",
        "the parameter must arrive length-prefixed and separate from the SQL, " +
        "got " + JSON.stringify(ext[0] && ext[0].params));
  check(ext[1] && ext[1].params[0] === null,
        "a null parameter must be -1, not an empty string, got " +
        JSON.stringify(ext[1] && ext[1].params));
  check(val("param") && (val("param").rows[0] || {}).p === "hello",
        "and the row comes back, got " + JSON.stringify(val("param")));
  const simple = good.queries.filter((q) => q.kind === "simple");
  check(simple.length >= 5 && simple.every((q) => !/\$1/.test(q.sql) || true),
        "unparameterised queries use the simple protocol, got " + simple.length);

  /* 8. THE CANCEL KEY IS SIZED FROM ITS MESSAGE */
  check(val("k32"), "the 32-octet-key backend must work too");
  check(D.k32.ready,
        "a 32-octet cancel key must not desynchronise the stream that follows");
  D.db.cancel();
  D.k32.cancel();

  /* 9. hostile framing */
  check(err("garbage") && /unknown message type/.test(emsg("garbage")),
        "an unknown type byte must be refused: " + emsg("garbage"));
  check(err("zero") && /length out of range/.test(emsg("zero")),
        "a length of 0 must be refused -- unchecked it makes the framing loop " +
        "rewind: " + emsg("zero"));
  check(err("huge") && /length out of range/.test(emsg("huge")),
        "a length past the cap must be refused: " + emsg("huge"));
  check(err("reject") && /pg_hba/.test(emsg("reject")),
        "a startup rejection must carry the server's reason: " + emsg("reject"));
  check(err("dead"), "a connect to a closed port must reject, not hang");

  /* 9b. the fixes that came out of the protocol review */
  check(err("noFinal") && /never proved it knows the password/.test(emsg("noFinal")),
        "a server that skips AuthenticationSASLFinal must be refused -- it " +
        "never proved it knows the password: " + emsg("noFinal"));
  check(err("twoSets") && /ONE statement per query/.test(emsg("twoSets")),
        "two result sets in one query must be refused, not merged into one " +
        "array whose shape changes partway through: " + emsg("twoSets"));
  check(err("twoDesc") && /ONE statement per query/.test(emsg("twoDesc")),
        "a SECOND RowDescription must be refused -- only the fields guard " +
        "sees this one: " + emsg("twoDesc"));
  check(err("twoTags") && /ONE statement per query/.test(emsg("twoTags")),
        "a SECOND CommandComplete with no new description must be refused -- " +
        "only the completion counter sees this one: " + emsg("twoTags"));
  check(err("shortRow") && /malformed DataRow|runs past/.test(emsg("shortRow")),
        "a DataRow that contradicts its own length must be refused, not " +
        "yield a short row: " + emsg("shortRow"));
  check(err("copy") && /COPY is not supported/.test(emsg("copy")),
        "COPY must be NAMED, not reported as an unknown message type: " +
        emsg("copy"));
  check(err("sevOnly") && err("sevOnly").severity === "FATAL",
        "severity must fall back to S when the server sends no V, got " +
        (err("sevOnly") && err("sevOnly").severity));
  check(D.db.parameters.__proto__ === "hijack",
        "a ParameterStatus named __proto__ must be a key, not a prototype, " +
        "got " + JSON.stringify(D.db.parameters.__proto__));
  {
    const ep = good.queries.filter((q) => q.kind === "extended" &&
                                          q.sql === "SELECT 1");
    check(ep.length === 1,
          "query(sql, []) must take the EXTENDED path -- the simple one runs " +
          "several statements -- got " + ep.length);
  }
  check(objErr !== null && /is an object/.test(objErr),
        "an object parameter must be refused, not stored as '[object " +
        "Object]', got " + objErr);
  check(arrErr !== null, "an array parameter must be refused, got " + arrErr);
  check(u8Err !== null && /is an object/.test(u8Err),
        "an object with no exact text form (a Date) must still be refused, " +
        "got " + u8Err);
  {
    /* The bytes on the wire, not the fact that the call succeeded: "1,255,0"
     * and "\\x01ff00" both send cleanly and only one is the bytea. */
    const b = ext.filter((q) => q.params.length === 1 &&
                                /^\\x/.test(q.params[0]));
    check(b.length === 1 && b[0].params[0] === "\\x01ff00",
          "a Uint8Array parameter must go out as the '\\x' hex bytea literal, " +
          "got " + JSON.stringify(b.map((q) => q.params[0])));
  }

  /* 10. synchronous refusals */
  check(tlsErr !== null && /TLS/.test(tlsErr),
        "tls:true must be refused by name, got " + tlsErr);
  check(paramErr !== null && /must be an array/.test(paramErr),
        "non-array parameters must be refused, got " + paramErr);

  setTimeout(() => {
    check(good.cancels.length === 1 && good.cancels[0].pid === 4242,
          "a cancel must open a FRESH connection and carry the pid, got " +
          JSON.stringify(good.cancels));
    check(good.cancels[0] && good.cancels[0].keyLen === 4,
          "with the 4-octet key this server sent, got " +
          (good.cancels[0] && good.cancels[0].keyLen));
    check(good32.cancels[0] && good32.cancels[0].keyLen === 32,
          "and 32 octets where the server sent 32 -- a fixed width would be " +
          "wrong on one of the two, got " +
          (good32.cancels[0] && good32.cancels[0].keyLen));

    for (const k in D) D[k].close();
    setTimeout(() => {
      check(good.terminated === true,
            "close() must send Terminate, or every disconnect is an " +
            "'unexpected EOF' in the server's log");
      good.close(); good32.close(); reject.close();
      garbage.close(); zeroLen.close(); hugeLen.close();
      noFinal.close(); twoSets.close(); shortRow.close();
      twoDesc.close(); twoTags.close();
      copyOut.close(); sevOnly.close();
      if (fails === 0) print("test_net_pg: all " + n + " checks passed");
      else print("test_net_pg: " + fails + " FAILED of " + n);
    }, 80);
    return;
  }, 150);
}, 10);
