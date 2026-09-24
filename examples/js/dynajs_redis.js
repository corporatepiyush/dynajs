// dynajs_redis.js — the dyna:net Redis client, driven against a mock server.
//
// dyna:net exposes Redis: a RESP2/RESP3 client that runs on the JS thread via
// the shared reactor. Every command returns a Promise, and replies are matched
// to commands by POSITION — so this example also shows the two places that
// matters: a pipeline is one promise for N replies, and a pub/sub delivery is
// out of band and never consumes a command's slot.
//
// No redis-server is required. The server here is a TCPServer speaking RESP,
// which is also the more useful thing to read: it shows the exact bytes.
//
// Run:  dynajs examples/js/dynajs_redis.js
//
// API:
//   const r = new Redis({ host, port, path, username, password, db, binary,
//                         maxReplyBytes, maxPending,
//                         connectTimeoutMs, commandTimeoutMs });
//   r.command(name, ...args)        -> Promise      args: string|number|Uint8Array
//   r.pipeline([[cmd, ...args]...]) -> Promise<Array>   one round trip
//   r.on("push"|"error", fn)        -> r
//   r.protocol  r.ready  r.pending  r.close()
//   An error REPLY rejects with an Error carrying .code (the first token:
//   "WRONGTYPE", "NOAUTH", "MOVED", ...) and .redis === true. A connection
//   failure rejects with .code === "CONNECTION".
//   TLS is not supported: `tls: true` throws rather than silently downgrading.

import { TCPServer, Redis } from "dyna:net";

let checks = 0, failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) { print("  FAIL: " + what); failures++; }
}

/* ---- a RESP server, in JS ------------------------------------------------ */

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
function bulk(s) {
  return s === null ? "$-1\r\n" : "$" + s.length + "\r\n" + s + "\r\n";
}

/* Parse one array-of-bulk-strings command; null when more bytes are needed. */
function parseCmd(s) {
  if (s.length === 0) return null;
  if (s[0] !== "*") throw new Error("mock: client sent a non-array command");
  const i = s.indexOf("\r\n");
  if (i < 0) return null;
  const count = parseInt(s.slice(1, i), 10);
  let p = i + 2;
  const args = [];
  for (let k = 0; k < count; k++) {
    if (p >= s.length) return null;
    if (s[p] !== "$") throw new Error("mock: argument is not a bulk string");
    const j = s.indexOf("\r\n", p);
    if (j < 0) return null;
    const len = parseInt(s.slice(p + 1, j), 10);
    if (s.length < j + 2 + len + 2) return null;
    args.push(s.slice(j + 2, j + 2 + len));
    p = j + 2 + len + 2;
  }
  return { args: args, used: p };
}

function makeServer(reply) {
  const bufs = new Map();
  const srv = new TCPServer({ port: 0 });
  srv.commands = [];
  srv.start({
    data: (c, b) => {
      bufs.set(c, (bufs.get(c) || "") + latin1(b));
      for (;;) {
        const one = parseCmd(bufs.get(c));
        if (!one) return;
        bufs.set(c, bufs.get(c).slice(one.used));
        srv.commands.push(one.args);
        const out = reply(one.args, c);
        if (out !== null && out !== undefined) c.write(bytes(out));
      }
    },
    close: (c) => { bufs.delete(c); },
  });
  return srv;
}

/* The reply table. Everything this example prints comes from here. */
const store = new Map();
const srv = makeServer((a, c) => {
  switch (a[0].toUpperCase()) {
  case "HELLO":
    return "%3\r\n" + bulk("server") + bulk("mocky") +
                      bulk("proto")  + ":3\r\n" +
                      bulk("id")     + ":1\r\n";
  case "PING":  return "+PONG\r\n";
  case "SET":   store.set(a[1], a[2]); return "+OK\r\n";
  case "GET":   return bulk(store.has(a[1]) ? store.get(a[1]) : null);
  case "INCR":  return ":7\r\n";
  case "LPUSH": return "-WRONGTYPE Operation against a key holding the wrong " +
                       "kind of value\r\n";
  case "SUBSCRIBE":
    /* one confirmation PER CHANNEL, then an unsolicited delivery */
    for (let i = 1; i < a.length; i++)
      c.write(bytes(">3\r\n" + bulk("subscribe") + bulk(a[i]) + ":" + i + "\r\n"));
    c.write(bytes(">3\r\n" + bulk("message") + bulk(a[1]) + bulk("ship it")));
    return null;
  default:
    return "-ERR unknown command '" + a[0] + "'\r\n";
  }
});

/* ---- drive it ------------------------------------------------------------ */

async function main() {
  const r = new Redis({ host: "127.0.0.1", port: srv.port });

  // 1. one command at a time. HELLO 3 is already queued ahead of these, so
  //    nothing here can be answered by a server that has not been configured.
  const pong = await r.command("PING");
  await r.command("SET", "greeting", "hello");
  const got = await r.command("GET", "greeting");
  const miss = await r.command("GET", "absent");
  print("PING      ->", JSON.stringify(pong));
  print("GET hit   ->", JSON.stringify(got));
  print("GET miss  ->", JSON.stringify(miss), '(a missing key is null, not "")');
  print("protocol  ->", r.protocol, " ready:", r.ready);
  check(pong === "PONG", "PING");
  check(got === "hello", "GET round trip");
  check(miss === null, "a missing key is null");
  check(r.protocol === 3, "HELLO 3 was accepted");

  // 2. a number goes out as its decimal text; a Uint8Array as its bytes.
  await r.command("SET", "n", 12345);
  await r.command("SET", "raw", new Uint8Array([104, 105]));
  print("SET n     -> stored as", JSON.stringify(store.get("n")));
  print("SET raw   -> stored as", JSON.stringify(store.get("raw")));
  check(store.get("n") === "12345", "a number argument coerces to decimal text");
  check(store.get("raw") === "hi", "a Uint8Array argument goes out as bytes");

  // 3. a value that is itself a whole command. Length-prefixed bulk means this
  //    is data: the server parses ONE three-argument SET, never a FLUSHALL.
  const evil = "x\r\n*1\r\n$8\r\nFLUSHALL\r\n";
  await r.command("SET", "evil", evil);
  const back = await r.command("GET", "evil");
  const flushes = srv.commands.filter((c) => c[0].toUpperCase() === "FLUSHALL");
  print("injection -> round-tripped verbatim:", back === evil,
        " FLUSHALL commands the server parsed:", flushes.length);
  check(back === evil, "a value containing a command round-trips verbatim");
  check(flushes.length === 0, "the server never parses a command out of a value");

  // 4. one round trip, one promise, replies in order.
  const batch = await r.pipeline([
    ["SET", "a", "1"],
    ["SET", "b", "2"],
    ["GET", "a"],
    ["INCR", "counter"],
  ]);
  print("pipeline  ->", JSON.stringify(batch));
  check(batch.length === 4, "a pipeline yields one element per command");
  check(batch[0] === "OK" && batch[2] === "1" && batch[3] === 7,
        "pipeline replies are in order");

  // 5. an error reply rejects and carries its class as .code, so a caller can
  //    branch without matching the message text.
  try {
    await r.command("LPUSH", "greeting", "x");
    check(false, "an error reply must reject");
  } catch (e) {
    print("error     -> code:", JSON.stringify(e.code),
          " redis:", e.redis, " message:", JSON.stringify(e.message));
    check(e instanceof Error, "an error reply rejects with an Error");
    check(e.code === "WRONGTYPE", "the class is the first token");
    check(e.redis === true, "and it is marked as coming from the server");
  }

  // 6. pub/sub. A subscribe answers once PER CHANNEL, so the client counts
  //    that many replies — counting one would leave every later reply a step
  //    out of phase. The message that follows is unsolicited and goes to the
  //    push handler instead.
  let deliver;
  const delivered = new Promise((res) => { deliver = res; });
  r.on("push", (m) => { deliver(m); });
  const confirm = await r.command("SUBSCRIBE", "news", "jobs");
  const msg = await delivered;
  print("subscribe -> two channels, two confirmations:", JSON.stringify(confirm));
  print("push      ->", JSON.stringify(msg));
  check(confirm.length === 2, "one confirmation per channel");
  check(confirm[1][1] === "jobs", "and the second names the second channel");
  check(msg[0] === "message" && msg[1] === "news" && msg[2] === "ship it",
        "the delivery reaches the push handler");

  // A PING is still legal while subscribed; a GET is not, on RESP2. This
  // connection is RESP3, where the restriction does not apply.
  check(await r.command("PING") === "PONG", "PING works while subscribed");

  r.close();

  // 7. binary: true returns bulk replies as bytes rather than a string, for
  //    values that are not text and would not survive a UTF-8 decode.
  const rb = new Redis({ host: "127.0.0.1", port: srv.port, binary: true });
  const raw = await rb.command("GET", "raw");
  print("binary    ->", Object.prototype.toString.call(raw),
        JSON.stringify(Array.from(raw)));
  check(raw instanceof Uint8Array, "binary:true gives a Uint8Array");
  check(raw.length === 2 && raw[0] === 104 && raw[1] === 105,
        "and the bytes are the ones that were stored");
  rb.close();

  // 8. TLS is refused by name. Connecting in plaintext to an endpoint the
  //    caller asked for in TLS is worse than not working.
  let refused = null;
  try { new Redis({ port: 6379, tls: true }); }
  catch (e) { refused = e.message; }
  print("tls:true  ->", JSON.stringify(refused));
  check(refused !== null && /TLS/.test(refused), "tls:true is refused by name");
}

main().then(() => {
  srv.close();
  print(failures === 0
    ? "dynajs_redis: all " + checks + " checks passed"
    : "dynajs_redis: " + failures + " FAILED of " + checks);
}, (e) => {
  srv.close();
  print("dynajs_redis: threw:", e && e.message ? e.message : String(e));
});
