// flags: --std
// covers: integration dyna:postgres.PostgreSQL.copyFrom dyna:postgres.PostgreSQL.copyTo -- COPY IN and COPY OUT share one gate, abort and resync path, so one suite drives that state machine
// PG protocol 54.2.6: copy-in drops Sync/Flush, any other message aborts it, so later queries are gated until the COPY ends; CopyBoth is replication-only and refused by name.
// Gap: under CONFIG_IO_URING the copy-in pump cannot peek the socket (copy_rx_ready is 0), so an endless sync source only sees a mid-copy server error once the send queue fills; that case is skipped when IO_URING=1 on Linux.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_copy");
import * as std from "std";
import * as os from "os";
import { ensurePostgres, stopStarted, PG_PORT } from "../lib/services.js";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function settle(p) { try { return { ok: await p }; } catch (e) { return { err: e }; } }
const em = (r) => r && r.err ? String(r.err.message || r.err) : "(resolved " + JSON.stringify(r && r.ok) + ")";
function syncThrow(f) { try { f(); return null; } catch (e) { return e; } }
const dec = new TextDecoder(), enc = new TextEncoder();

const CFG = { host: "127.0.0.1", port: PG_PORT, user: "postgres", password: "pw", database: "djtest" };
const svc = __kitS.listing ? null : await ensurePostgres({ port: PG_PORT, user: "postgres", password: "pw", database: "djtest" });
let db = null;
try {

const latin1 = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const str = (u8) => { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; };
const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
const i16 = (v) => String.fromCharCode((v >>> 8) & 255, v & 255);
const msg = (t, b) => t + i32(b.length + 4) + b;

function fakeServer(onQuery) {
  const bufs = new Map(), started = new Map(), log = [];
  const srv = new TCPServer({ port: 0 });
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + str(b));
      for (;;) {
        const s = bufs.get(c);
        if (!started.get(c)) {
          if (s.length < 8) return;
          const len = ((s.charCodeAt(0) << 24) | (s.charCodeAt(1) << 16) | (s.charCodeAt(2) << 8) | s.charCodeAt(3)) >>> 0;
          if (s.length < len) return;
          bufs.set(c, s.slice(len)); started.set(c, true);
          c.write(latin1(msg("R", i32(0)) + msg("K", i32(9) + "abcd") + msg("Z", "I")));
          continue;
        }
        if (s.length < 5) return;
        const len = ((s.charCodeAt(1) << 24) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 8) | s.charCodeAt(4)) >>> 0;
        if (s.length < len + 1) return;
        const type = s[0], body = s.slice(5, len + 1);
        bufs.set(c, s.slice(len + 1));
        log.push(type === "Q" ? "Q:" + body.slice(0, -1) : type);
        const out = onQuery(type, body, log);
        if (out) try { c.write(latin1(out)); } catch (e) {}
      }
    },
    close: (c) => { bufs.delete(c); started.delete(c); },
  });
  return { srv, log };
}

await __kitS.section("fake server: CopyBoth refused by name, stray CopyData fails, malformed CopyInResponse fails", async () => {
  const cases = [
    ["W", msg("W", "\0" + i16(0)), /CopyBothResponse/],
    ["d", msg("d", "x") + msg("C", "SELECT 0\0") + msg("Z", "I"), /CopyData outside/],
    ["c", msg("c", "") + msg("Z", "I"), /CopyDone outside/],
    ["G2", msg("G", "\x02" + i16(0)), /malformed COPY response/],
    ["Gtrunc", msg("G", "\0" + i16(3) + i16(0)), /malformed COPY response/],
    ["Gtextbin", msg("G", "\0" + i16(1) + i16(1)), /malformed COPY response/],
  ];
  for (const [name, reply, re] of cases) {
    const { srv } = fakeServer((t) => t === "Q" ? reply : null);
    const c = new PostgreSQL({ host: "127.0.0.1", port: srv.port, user: "u" });
    const r = await settle(c.query("SELECT 1"));
    check(r.err && re.test(em(r)), name + ": rejects naming the fault (" + em(r) + ")");
    check(c.closed || r.err.code === "CONNECTION", name + ": connection failed with CONNECTION");
    c.close(); srv.close();
  }
});

await __kitS.section("fake server: COPY gate holds later queries until CopyDone, then pipelines them", async () => {
  const { srv, log } = fakeServer((t, body, lg) => {
    if (t === "Q" && /^COPY/.test(body)) return msg("G", "\0" + i16(1) + i16(0));
    if (t === "c") return msg("C", "COPY 2\0") + msg("Z", "I");
    if (t === "f") return msg("E", "SERROR\0C57014\0M" + body.slice(0, -1) + "\0\0") + msg("Z", "I");
    if (t === "Q") return msg("C", "SELECT 0\0") + msg("Z", "I");
    return null;
  });
  const c = new PostgreSQL({ host: "127.0.0.1", port: srv.port, user: "u" });
  await c.query("SELECT 0");
  async function* slow() { yield "1\n"; await sleep(30); yield "2\n"; }
  const p1 = c.copyFrom("COPY t FROM STDIN", slow());
  const p2 = c.query("SELECT 1");
  const p3 = c.query("SELECT 2");
  const [r1, r2, r3] = await Promise.all([settle(p1), settle(p2), settle(p3)]);
  check(r1.ok && r1.ok.rowCount === 2 && r1.ok.command === "COPY 2", "copyFrom resolves {command COPY, rowCount 2}: " + em(r1));
  check(r2.ok && r3.ok, "queries behind the COPY resolve");
  const seq = log.filter((x) => x !== "S").join(",");
  check(seq === "Q:SELECT 0,Q:COPY t FROM STDIN,d,d,c,Q:SELECT 1,Q:SELECT 2", "nothing but CopyData/CopyDone reaches the server during copy-in: " + seq);
  log.length = 0;
  const p4 = c.query("COPY t FROM STDIN");
  const p5 = c.query("SELECT 3");
  const r4 = await settle(p4), r5 = await settle(p5);
  check(r4.err && /copyFrom/.test(em(r4)), "query() on COPY FROM STDIN sends CopyFail and rejects with a copyFrom hint: " + em(r4));
  check(r5.ok, "the query after the refused COPY still runs");
  check(log.filter((x) => x !== "S").join(",") === "Q:COPY t FROM STDIN,f,Q:SELECT 3", "CopyFail then the next query: " + log.join(","));
  c.close(); srv.close();
});

if (svc && svc.ok) {
  try { db = new PostgreSQL(CFG); await db.query("SELECT 1"); } catch (e) { print("test_postgres_copy: SKIP live part: " + e.message); if (db) db.close(); db = null; }
} else if (!__kitS.listing) print("test_postgres_copy: SKIP live part: " + (svc ? svc.why : "listing"));

async function count(t) { return Number((await db.query("SELECT count(*)::int AS n FROM " + t)).rows[0].n); }

await __kitS.section("copyFrom sources: string, Uint8Array, ArrayBuffer, sync and async iterables, empty", async () => {
  if (!db) return;
  await db.query("CREATE TEMP TABLE cf(a int, b text)");
  let r = await db.copyFrom("COPY cf FROM STDIN", "1\tx\n2\ty\n");
  check(r.command === "COPY 2" && r.rowCount === 2, "string source copies 2 rows: " + JSON.stringify(r));
  r = await db.copyFrom("COPY cf FROM STDIN", enc.encode("3\tz\n"));
  check(r.rowCount === 1, "Uint8Array source");
  r = await db.copyFrom("COPY cf FROM STDIN", enc.encode("4\tw\n").buffer);
  check(r.rowCount === 1, "ArrayBuffer source");
  const big = enc.encode("xx5\tv\nyy");
  r = await db.copyFrom("COPY cf FROM STDIN", big.subarray(2, 6));
  check(r.rowCount === 1, "a subarray view sends only its window");
  r = await db.copyFrom("COPY cf FROM STDIN", (function* () { yield "6\t"; yield enc.encode("a\n7\tb"); yield "\n"; })());
  check(r.rowCount === 2, "sync generator chunks split mid-row");
  r = await db.copyFrom("COPY cf FROM STDIN", (async function* () { for (let i = 8; i < 18; i++) { if (i % 3 === 0) await sleep(1); yield i + "\tq\n"; } })());
  check(r.rowCount === 10, "async generator source");
  r = await db.copyFrom("COPY cf FROM STDIN", "");
  check(r.rowCount === 0, "empty string copies 0 rows");
  r = await db.copyFrom("COPY cf FROM STDIN", (function* () {})());
  check(r.rowCount === 0, "empty iterable copies 0 rows");
  r = await db.copyFrom("COPY cf FROM STDIN", ["18\tarr\n", "19\tarr\n"]);
  check(r.rowCount === 2, "an array is an iterable source");
  r = await db.copyFrom("COPY cf (a, b) FROM STDIN WITH (FORMAT csv)", "20,\"x,y\"\n");
  check(r.rowCount === 1 && (await db.query("SELECT b FROM cf WHERE a = 20")).rows[0].b === "x,y", "CSV format");
  check(await count("cf") === 20, "table holds every copied row");
});

await __kitS.section("copyFrom large source crosses 1 MiB frames and highWaterMark pacing", async () => {
  if (!db) return;
  await db.query("CREATE TEMP TABLE big(a int, b text)");
  const pad = "p".repeat(100);
  let one = ""; for (let i = 0; i < 12000; i++) one += i + "\t" + pad + "\n";
  let r = await db.copyFrom("COPY big FROM STDIN", one);
  check(r.rowCount === 12000, "1.2 MB string in one call: " + r.rowCount);
  let made = 0;
  r = await db.copyFrom("COPY big FROM STDIN", (function* () { for (let k = 0; k < 400; k++) { let s = ""; for (let i = 0; i < 500; i++) s += (k * 500 + i) + "\t" + pad + "\n"; made += 500; yield s; } })(), { highWaterMark: 4096 });
  check(r.rowCount === 200000 && made === 200000, "200k rows through a 4 KiB highWaterMark: " + r.rowCount);
});

await __kitS.section("copyFrom failures keep the connection usable", async () => {
  if (!db) return;
  await db.query("CREATE TEMP TABLE ff(a int)");
  const boom = new Error("source boom");
  let closed = 0;
  let r = await settle(db.copyFrom("COPY ff FROM STDIN", (function* () { try { yield "1\n"; throw boom; } finally { closed++; } })()));
  check(r.err === boom, "a throwing source rejects with its own error: " + em(r));
  check((await db.query("SELECT 1 AS x")).rows[0].x === 1, "connection usable after a throwing source");
  check(await count("ff") === 0, "the aborted COPY wrote nothing");
  r = await settle(db.copyFrom("COPY ff FROM STDIN", (async function* () { yield "1\n"; await sleep(1); throw boom; })()));
  check(r.err === boom, "an async source rejection rejects with its error");
  r = await settle(db.copyFrom("COPY ff FROM STDIN", [42]));
  check(r.err instanceof TypeError && /not a string/.test(em(r)), "a non-bytes chunk rejects with TypeError: " + em(r));
  r = await settle(db.copyFrom("COPY ff FROM STDIN", "abc\n"));
  check(r.err && r.err.code === "22P02", "bad data rejects with the server error 22P02: " + (r.err && r.err.code));
  closed = 0;
  r = await settle(db.copyFrom("COPY ff FROM STDIN", (async function* () { try { for (let i = 0; ; i++) { if (i % 50 === 49) await sleep(1); yield i === 5 ? "bad\n" : i + "\n"; } } finally { closed++; } })()));
  check(r.err && r.err.code === "22P02", "server error mid-stream rejects: " + em(r));
  await db.query("SELECT 1");
  for (let i = 0; i < 20 && closed === 0; i++) await sleep(5);
  check(closed === 1, "the source iterator is closed once after a server error (" + closed + ")");
  if (os.platform === "linux" && std.getenv("IO_URING") === "1") print("test_postgres_copy: SKIP endless-source server-error case: io_uring build has no receive peek");
  else {
  closed = 0;
  const endless = db.copyFrom("COPY ff FROM STDIN", (function* () { try { for (let i = 0; ; i++) yield i === 5 ? "bad\n" : i + "\n"; } finally { closed++; } })());
  r = await settle(Promise.race([endless, sleep(8000).then(() => { throw new Error("pump never saw the server error"); })]));
  check(r.err && r.err.code === "22P02", "an endless sync source stops at a mid-copy server error: " + em(r));
  check(closed === 1, "the endless source is closed once (" + closed + ")");
  }
  r = await settle(db.copyFrom("COPY ff FROM STDIN", "1\u0000\n"));
  check(!!r.err, "a NUL byte in text COPY data is a server-side error: " + em(r));
  r = await settle(db.copyFrom("COPY nosuch FROM STDIN", (function* () { closed = -100; yield "1\n"; })()));
  check(r.err && r.err.code === "42P01" && closed !== -100, "an error before CopyInResponse never pulls the source");
  r = await settle(db.copyFrom("COPY (SELECT 1) TO STDOUT", "x"));
  check(r.err && /copyTo/.test(em(r)), "copyFrom with COPY TO STDOUT rejects with a copyTo hint: " + em(r));
  r = await settle(db.query("COPY ff FROM STDIN"));
  check(r.err && /copyFrom/.test(em(r)), "query() with COPY FROM STDIN rejects with a hint: " + em(r));
  r = await settle(db.query("  /* c */ -- x\n copy (select 1) TO STDOUT"));
  check(r.err && /copyTo/.test(em(r)), "query() with COPY TO STDOUT rejects with a hint: " + em(r));
  check((await db.query("SELECT 2 AS x")).rows[0].x === 2, "connection usable after every failure");
});

await __kitS.section("copyFrom into a table whose trigger raises a NOTICE per row (plain and TLS)", async () => {
  if (!db) return;
  await db.query("CREATE OR REPLACE FUNCTION pg_temp.nt_f() RETURNS trigger LANGUAGE plpgsql AS \$\$BEGIN RAISE NOTICE 'row %', NEW.a; RETURN NEW; END\$\$");
  await db.query("CREATE TEMP TABLE nt(a int)");
  await db.query("CREATE TRIGGER nt_t BEFORE INSERT ON nt FOR EACH ROW EXECUTE FUNCTION pg_temp.nt_f()");
  let notes = 0;
  db.on("notice", () => { notes++; });
  const t0 = Date.now();
  const r = await settle(db.copyFrom("COPY nt FROM STDIN", (function* () { for (let i = 0; i < 20000; i++) yield i + "\n"; })()));
  check(r.ok && r.ok.rowCount === 20000, "20000 rows with a NOTICE each complete: " + em(r) + " in " + (Date.now() - t0) + " ms");
  check(notes === 20000, "every interleaved NoticeResponse reached the handler (" + notes + ")");
  db.on("notice", null);
  if (!svc.ca) { print("test_postgres_copy: SKIP TLS NOTICE variant: no CA"); return; }
  const t = new PostgreSQL({ ...CFG, tls: true, ca: svc.ca });
  try {
    await t.query("CREATE OR REPLACE FUNCTION pg_temp.nt_f() RETURNS trigger LANGUAGE plpgsql AS \$\$BEGIN RAISE NOTICE 'row %', NEW.a; RETURN NEW; END\$\$");
    await t.query("CREATE TEMP TABLE nt(a int)");
    await t.query("CREATE TRIGGER nt_t BEFORE INSERT ON nt FOR EACH ROW EXECUTE FUNCTION pg_temp.nt_f()");
    let tn = 0;
    t.on("notice", () => { tn++; });
    const r2 = await settle(t.copyFrom("COPY nt FROM STDIN", (function* () { for (let i = 0; i < 20000; i++) yield i + "\n"; })()));
    check(r2.ok && r2.ok.rowCount === 20000 && tn === 20000, "TLS: 20000 rows with NOTICEs complete (" + em(r2) + ", " + tn + " notices)");
    const ssl = await t.query("SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()");
    check(ssl.rows[0].ssl === true, "the TLS variant really ran over TLS");
  } finally { t.close(); }
});

await __kitS.section("queries pipelined behind a live COPY see its rows", async () => {
  if (!db) return;
  await db.query("CREATE TEMP TABLE pq(a int)");
  const p1 = db.copyFrom("COPY pq FROM STDIN", (async function* () { for (let i = 0; i < 50; i++) { await sleep(0); yield i + "\n"; } })());
  const p2 = db.query("SELECT count(*)::int AS n FROM pq");
  const p3 = db.pipeline ? db.pipeline([["SELECT 1 AS x"]]) : db.query("SELECT 1 AS x");
  const [r1, r2, r3] = await Promise.all([settle(p1), settle(p2), settle(p3)]);
  check(r1.ok && r1.ok.rowCount === 50, "the COPY completes: " + em(r1));
  check(r2.ok && r2.ok.rows[0].n === 50, "the next query runs after it: " + em(r2));
  check(!!r3.ok, "a pipeline behind the COPY runs: " + em(r3));
});

await __kitS.section("copyTo streams rows, done value, return(), errors", async () => {
  if (!db) return;
  const it = db.copyTo("COPY (SELECT g, 'r' || g FROM generate_series(1, 5) g) TO STDOUT");
  check(it[Symbol.asyncIterator]() === it, "the stream is its own async iterator");
  let text = "", chunks = 0, last;
  for (;;) { const s = await it.next(); if (s.done) { last = s.value; break; } check(s.value instanceof Uint8Array, "chunk is a Uint8Array"); text += dec.decode(s.value); chunks++; }
  check(text === "1\tr1\n2\tr2\n3\tr3\n4\tr4\n5\tr5\n", "copyTo text matches: " + JSON.stringify(text));
  check(chunks === 5 && last && last.command === "COPY 5" && last.rowCount === 5, "done value carries command and rowCount: " + JSON.stringify(last));
  check((await it.next()).done, "next() after done stays done");
  let out = "";
  for await (const ch of db.copyTo("COPY (SELECT 1, 'a') TO STDOUT WITH (FORMAT csv, HEADER)")) out += dec.decode(ch);
  check(out === "?column?,?column?\n1,a\n", "for await over CSV with header: " + JSON.stringify(out));
  const bin = []; for await (const ch of db.copyTo("COPY (SELECT 1) TO STDOUT (FORMAT binary)")) bin.push(...ch);
  check(str(new Uint8Array(bin.slice(0, 11))) === "PGCOPY\n\xff\r\n\0", "binary COPY output starts with the PGCOPY signature");
  const early = db.copyTo("COPY (SELECT g FROM generate_series(1, 100000) g) TO STDOUT", { highWaterMark: 1024 });
  const f1 = await early.next();
  check(!f1.done && dec.decode(f1.value) === "1\n", "first row arrives");
  await sleep(50);
  const ret = await early.return();
  check(ret.done === true, "return() resolves done");
  check((await early.next()).done, "next() after return() is done");
  check((await db.query("SELECT 3 AS x")).rows[0].x === 3, "connection usable after an early return()");
  let r = await settle((async () => { for await (const _ of db.copyTo("COPY nosuch TO STDOUT")) {} })());
  check(r.err && r.err.code === "42P01", "a server error rejects next(): " + em(r));
  r = await settle((async () => { for await (const _ of db.copyTo("COPY (SELECT 1/(g-3) FROM generate_series(1, 5) g) TO STDOUT", {})) {} })());
  check(r.err && r.err.code === "22012", "an error mid-stream rejects next(): " + em(r));
  r = await settle((async () => { for await (const _ of db.copyTo("COPY ff FROM STDIN")) {} })());
  check(r.err && /copyFrom/.test(em(r)), "copyTo with COPY FROM STDIN rejects with a copyFrom hint: " + em(r));
  let te = syncThrow(() => db.copyTo("SELECT 1"));
  check(te instanceof TypeError && /copyTo/.test(te.message), "copyTo on non-COPY SQL throws a TypeError naming copyTo: " + te);
  te = syncThrow(() => db.copyTo("  /* x */ DELETE FROM ff"));
  check(te instanceof TypeError, "copyTo refuses a non-COPY statement before sending it");
  r = await settle(db.copyTo("COPY (SELECT 1) TO PROGRAM 'cat > /dev/null'").next());
  check(r.err instanceof TypeError && /without COPY TO STDOUT/.test(em(r)), "a COPY that never streams rejects next() with TypeError: " + em(r));
  const pend = db.copyTo("COPY (SELECT 1) TO STDOUT");
  const n1 = pend.next();
  check(syncThrow(() => pend.next()) instanceof TypeError, "a second concurrent next() throws TypeError");
  await n1; for await (const _ of pend) {}
  check((await db.query("SELECT 4 AS x")).rows[0].x === 4, "connection usable after copyTo errors");
});

await __kitS.section("copyTo backpressure: a slow reader receives every row", async () => {
  if (!db) return;
  const st = db.copyTo("COPY (SELECT g, repeat('z', 200) FROM generate_series(1, 20000) g) TO STDOUT", { highWaterMark: 2048 });
  let rows = 0, k = 0;
  for await (const ch of st) { rows += dec.decode(ch).split("\n").length - 1; if (++k % 4000 === 0) await sleep(20); }
  check(rows === 20000, "every row after pauses: " + rows);
  const side = await db.query("SELECT 5 AS x");
  check(side.rows[0].x === 5, "connection usable after a paused stream");
});

await __kitS.section("rule 18: hostile options, getters, Proxies, limits, GC, closed", async () => {
  if (!db) return;
  await db.query("CREATE TEMP TABLE h(a int)");
  const boom = new Error("getter boom");
  let e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", "1\n", { get highWaterMark() { throw boom; } }));
  check(e === boom, "a throwing highWaterMark getter propagates");
  e = syncThrow(() => db.copyTo("COPY (SELECT 1) TO STDOUT", { highWaterMark: { valueOf() { throw boom; } } }));
  check(e === boom, "a throwing valueOf propagates");
  for (const v of [0, -1, 2 ** 30 + 1, 1.5, NaN, Infinity]) {
    e = syncThrow(() => db.copyTo("COPY (SELECT 1) TO STDOUT", { highWaterMark: v }));
    check(e instanceof RangeError, "highWaterMark " + v + " is a RangeError");
  }
  let r = await db.copyFrom("COPY h FROM STDIN", "1\n", { highWaterMark: 2 ** 30 });
  check(r.rowCount === 1, "highWaterMark 2^30 accepted");
  r = await db.copyFrom("COPY h FROM STDIN", "2\n", { highWaterMark: 1 });
  check(r.rowCount === 1, "highWaterMark 1 accepted");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", "1\n", { highWatermark: 5 }));
  check(e instanceof TypeError, "an unknown option is refused");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", "1\n", new Proxy({}, { ownKeys() { throw boom; } })));
  check(e === boom, "a Proxy options trap error propagates");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", "1\n", 5));
  check(e instanceof TypeError, "non-object options are refused");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN\0x", "1\n"));
  check(e instanceof TypeError && /NUL/.test(e.message), "NUL in sql is refused");
  e = syncThrow(() => db.copyFrom({ toString() { return "COPY h FROM STDIN"; } }, "1\n"));
  check(e instanceof TypeError, "non-string sql is refused without calling toString");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", 42));
  check(e instanceof TypeError, "a number source is refused");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", { get [Symbol.asyncIterator]() { throw boom; } }));
  check(e === boom, "a throwing Symbol.asyncIterator getter propagates");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", { [Symbol.iterator]: 1 }));
  check(e instanceof TypeError, "a non-callable Symbol.iterator is refused");
  e = syncThrow(() => db.copyFrom("COPY h FROM STDIN", { [Symbol.iterator]() { return { next: 1 }; } }));
  check(e instanceof TypeError, "an iterator without next() is refused");
  r = await settle(db.copyFrom("COPY h FROM STDIN", { [Symbol.iterator]() { return { next() { return { get done() { throw boom; } }; } }; } }));
  check(r.err === boom, "a throwing done getter rejects with its error");
  r = await settle(db.copyFrom("COPY h FROM STDIN", { [Symbol.iterator]() { return { next() { return { done: false, get value() { throw boom; } }; } }; } }));
  check(r.err === boom, "a throwing value getter rejects with its error");
  r = await settle(db.copyFrom("COPY h FROM STDIN", { [Symbol.iterator]() { return { next() { return 7; } }; } }));
  check(r.err instanceof TypeError, "a non-object iterator result rejects with TypeError");
  let k = 0;
  const px = new Proxy({ [Symbol.iterator]() { return { next() { return k < 3 ? { done: false, value: (10 + k++) + "\n" } : { done: true }; } }; } }, {});
  r = await db.copyFrom("COPY h FROM STDIN", px);
  check(r.rowCount === 3, "a Proxy-wrapped iterable works");
  r = await db.copyFrom("COPY h FROM STDIN", (function* () { for (let i = 0; i < 300; i++) { std.gc(); yield i + "\n"; } })());
  check(r.rowCount === 300, "GC on every pull keeps the copy intact");
  const ab = new ArrayBuffer(8, { maxByteLength: 16 });
  r = await settle(db.copyFrom("COPY h FROM STDIN", (function* () { const u = new Uint8Array(ab, 0, 2); u.set([0x39, 0x0a]); ab.resize(0); yield u; })()));
  check(!!r.ok || !!r.err, "a chunk whose buffer shrank to zero settles: " + em(r));
  let st = db.copyTo("COPY (SELECT g FROM generate_series(1, 2000) g) TO STDOUT", { highWaterMark: 16 });
  await st.next(); st = null;
  for (let i = 0; i < 5; i++) { std.gc(); await sleep(5); }
  check((await db.query("SELECT 6 AS x")).rows[0].x === 6, "a dropped, paused copyTo stream is collected and the connection drains");
  const c2 = new PostgreSQL(CFG); await c2.query("SELECT 1");
  const it2 = c2.copyTo("COPY (SELECT pg_sleep(0.2)) TO STDOUT");
  const pIn = c2.copyFrom("COPY h FROM STDIN", (async function* () { await sleep(500); yield "1\n"; })());
  c2.close();
  r = await settle(it2.next());
  check(r.err && r.err.code === "CONNECTION", "close() rejects a pending copyTo next(): " + em(r));
  r = await settle(pIn);
  check(r.err && r.err.code === "CONNECTION", "close() rejects a pending copyFrom: " + em(r));
  const c3 = new PostgreSQL(CFG); await c3.query("CREATE TEMP TABLE hang(a int)");
  let pulls = 0;
  const pHang = c3.copyFrom("COPY hang FROM STDIN", { [Symbol.asyncIterator]() { return { next() { pulls++; return new Promise(() => {}); } }; } });
  for (let i = 0; i < 100 && !pulls; i++) await sleep(5);
  c3.close();
  r = await settle(pHang);
  check(pulls === 1 && r.err && r.err.code === "CONNECTION", "close() while a source next() never settles rejects and frees the copy: " + em(r));
  e = syncThrow(() => c2.copyFrom("COPY h FROM STDIN", "1\n"));
  check(e instanceof Error && /closed/.test(e.message), "copyFrom after close() throws");
  e = syncThrow(() => c2.copyTo("COPY h TO STDOUT"));
  check(e instanceof Error && /closed/.test(e.message), "copyTo after close() throws");
});

} catch (e) { check(false, "uncaught: " + (e && e.stack || e)); } finally {
  if (db) db.close();
  await stopStarted();
}
print("test_postgres_copy: " + (n - fails) + "/" + n + " passed");
if (fails) std.exit(1);
