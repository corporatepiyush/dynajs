import { TCPServer, Redis } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

{
    function throwsMsgC6(fn, re, m) {
        let threw = null;
        try { fn(); } catch (e) { threw = e; }
        check(threw !== null && threw instanceof TypeError && re.test(threw.message),
              m + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    }

    throwsMsgC6(() => new Redis({ host: "127.0.0.1", portz: 6379 }),
                /unknown option "portz" \(valid: tls, ca, host, path, username, password, port, db, binary, bigint, maxReplyBytes, maxPending, connectTimeoutMs, commandTimeoutMs\)/,
                "Redis ctor bag");
}

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

function parseCmd(s) {
  if (s.length === 0) return null;
  if (s[0] !== "*") throw new Error("mock: client sent a non-array command");
  let i = s.indexOf("\r\n");
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
        let one;
        try { one = parseCmd(bufs.get(c)); }
        catch (e) { srv.parseError = String(e); bufs.set(c, ""); return; }
        if (!one) return;
        bufs.set(c, bufs.get(c).slice(one.used));
        srv.commands.push(one.args);
        const out = reply(one.args, c, srv);
        if (out !== null && out !== undefined) c.write(bytes(out));
      }
    },
    close: (c) => { bufs.delete(c); },
  });
  return srv;
}

function bulk(s) { return s === null ? "$-1\r\n" : "$" + s.length + "\r\n" + s + "\r\n"; }

const store = new Map();
const modern = makeServer((a, c, srv) => {
  const cmd = a[0].toUpperCase();
  if (cmd === "HELLO")
    return "%3\r\n$6\r\nserver\r\n$5\r\nmocky\r\n$5\r\nproto\r\n:3\r\n" +
           "$2\r\nid\r\n:7\r\n";
  if (cmd === "PING") return "+PONG\r\n";
  if (cmd === "SET") { store.set(a[1], a[2]); return "+OK\r\n"; }
  if (cmd === "GET") return bulk(store.has(a[1]) ? store.get(a[1]) : null);
  if (cmd === "INCR") return ":42\r\n";
  if (cmd === "BOOM") return "-ERR the command failed\r\n";
  if (cmd === "NOPE") return "-WRONGTYPE not a list\r\n";
  if (cmd === "LRANGE") return "*2\r\n" + bulk("a") + bulk("b");
  if (cmd === "MAP") return "%1\r\n" + bulk("k") + ":9\r\n";
  if (cmd === "BIGNUM") return "(12345678901234567890123\r\n";
  if (cmd === "DBL") return ",3.5\r\n";
  if (cmd === "NIL") return "_\r\n";
  if (cmd === "TRUE") return "#t\r\n";
  if (cmd === "SUBSCRIBE" && a.length === 2) {
    c.write(bytes(">3\r\n" + bulk("subscribe") + bulk(a[1]) + ":1\r\n"));
    c.write(bytes(">3\r\n" + bulk("message") + bulk(a[1]) + bulk("hello-there")));
    return null;
  }
  if (cmd === "BIGINT") return ":9007199254740993\r\n";
  if (cmd === "SMALLINT") return ":9007199254740992\r\n";
  if (cmd === "INFO") return "=15\r\ntxt:Some string\r\n";
  if (cmd === "ATTRED") return "|1\r\n" + bulk("ttl") + ":60\r\n+real\r\n";
  if (cmd === "PROTOKEY") return "%1\r\n" + bulk("__proto__") + ":5\r\n";
  if (cmd === "BINMAP") return "%1\r\n" + bulk("maxmemory") + bulk("0");
  if (cmd === "SUBSCRIBE" && a.length > 2) {
    for (let i = 1; i < a.length; i++)
      c.write(bytes(">3\r\n" + bulk("subscribe") + bulk(a[i]) + ":" + i + "\r\n"));
    return null;
  }
  if (cmd === "SLOW") return null;
  return "-ERR unknown command '" + a[0] + "'\r\n";
});

const legacy = makeServer((a) => {
  const cmd = a[0].toUpperCase();
  if (cmd === "HELLO") return "-ERR unknown command 'HELLO'\r\n";
  if (cmd === "AUTH") return a[a.length - 1] === "s3cret"
        ? "+OK\r\n" : "-WRONGPASS bad password\r\n";
  if (cmd === "SELECT") return "+OK\r\n";
  if (cmd === "PING") return "+PONG\r\n";
  return "-ERR unknown\r\n";
});

function hostile(after) {
  return makeServer((a, c) => {
    if (a[0].toUpperCase() === "HELLO") return "%1\r\n" + bulk("proto") + ":3\r\n";
    return after(a, c);
  });
}
const srvTls      = hostile(() => "\x16\x03\x01\x00\x50" + "junkjunkjunk");
const srvGarbage  = hostile(() => "@not-a-type\r\n");
const srvHugeCnt  = hostile(() => "*2000000000\r\n");
const srvExtra    = hostile((a, c) => { c.write(bytes("+OK\r\n")); return "+OK\r\n"; });
const srvDeepNest = hostile(() => "*1\r\n".repeat(40) + ":1\r\n");
const srvDeepOk   = hostile(() => "*1\r\n".repeat(31) + ":7\r\n");
const srvDeep300  = hostile(() => "*1\r\n".repeat(300) + ":7\r\n");
const srvAttrChain = hostile(() => "|1\r\n$1\r\nk\r\n:0\r\n".repeat(300) + ":7\r\n");

const R = {};
const results = {};
function rec(k, p) { results[k] = "pending";
  p.then((v) => { results[k] = { ok: v }; },
         (e) => { results[k] = { err: e }; }); }

R.modern = new Redis({ port: modern.port, host: "127.0.0.1" });
rec("ping", R.modern.command("PING"));
rec("set", R.modern.command("SET", "k", "v"));
rec("get", R.modern.command("GET", "k"));
rec("miss", R.modern.command("GET", "absent"));
rec("incr", R.modern.command("INCR", "c"));
rec("boom", R.modern.command("BOOM"));
rec("wrongtype", R.modern.command("NOPE"));
rec("lrange", R.modern.command("LRANGE", "l", 0, -1));
rec("map", R.modern.command("MAP"));
rec("bignum", R.modern.command("BIGNUM"));
rec("dbl", R.modern.command("DBL"));
rec("nil", R.modern.command("NIL"));
rec("true", R.modern.command("TRUE"));
rec("pipe", R.modern.pipeline([["PING"], ["SET", "p", "1"], ["GET", "p"]]));

const EVIL = "v\r\n*1\r\n$8\r\nFLUSHALL\r\n";
rec("evilset", R.modern.command("SET", "evil", EVIL));
rec("evilget", R.modern.command("GET", "evil"));

rec("numarg", R.modern.command("SET", "num", 12345));
rec("bytearg", R.modern.command("SET", "bin", new Uint8Array([104, 105])));

let pushes = [];
R.modern.on("push", (m) => { pushes.push(m); });
rec("sub", R.modern.command("SUBSCRIBE", "chan"));

rec("bigint", R.modern.command("BIGINT"));
rec("smallint", R.modern.command("SMALLINT"));
rec("info", R.modern.command("INFO"));
rec("attred", R.modern.command("ATTRED"));
rec("protokey", R.modern.command("PROTOKEY"));

let multiErr = null;
R.multi = new Redis({ port: modern.port, host: "127.0.0.1" });
R.multi.on("push", () => {});
rec("multisub", R.multi.command("SUBSCRIBE", "a", "b", "c"));
rec("aftersub", R.multi.command("PING"));
try { R.multi.command("UNSUBSCRIBE"); }
catch (e) { multiErr = String(e); }

R.bin = new Redis({ port: modern.port, host: "127.0.0.1", binary: true });
R.big = new Redis({ port: modern.port, host: "127.0.0.1", bigint: true });
rec("bigIntOpt", R.big.command("BIGINT"));
rec("smallIntOpt", R.big.command("SMALLINT"));
rec("binget", R.bin.command("GET", "k"));
rec("binmap", R.bin.command("BINMAP"));

R.legacy = new Redis({ port: legacy.port, host: "127.0.0.1",
                       password: "s3cret", db: 3 });
rec("legacyping", R.legacy.command("PING"));

R.badpass = new Redis({ port: legacy.port, host: "127.0.0.1",
                        password: "wrong" });
rec("badpass", R.badpass.command("PING"));

R.tls = new Redis({ port: srvTls.port, host: "127.0.0.1" });
rec("tls", R.tls.command("PING"));
R.garbage = new Redis({ port: srvGarbage.port, host: "127.0.0.1" });
rec("garbage", R.garbage.command("PING"));
R.huge = new Redis({ port: srvHugeCnt.port, host: "127.0.0.1" });
rec("huge", R.huge.command("PING"));
let extraErr = null;
R.extra = new Redis({ port: srvExtra.port, host: "127.0.0.1" });
R.extra.on("error", (e) => { if (!extraErr) extraErr = String(e.message || e); });
rec("extra", R.extra.command("PING").then(
  () => new Promise((res) => setTimeout(() => res(R.extra.ready), 120))));
R.deep = new Redis({ port: srvDeepNest.port, host: "127.0.0.1" });
rec("deep", R.deep.command("PING"));
R.deepOk = new Redis({ port: srvDeepOk.port, host: "127.0.0.1" });
rec("deepOk", R.deepOk.command("PING"));
R.deep300 = new Redis({ port: srvDeep300.port, host: "127.0.0.1" });
rec("deep300", R.deep300.command("PING"));
R.attrchain = new Redis({ port: srvAttrChain.port, host: "127.0.0.1" });
rec("attrchain", R.attrchain.command("PING"));

R.dead = new Redis({ port: 1, host: "127.0.0.1" });
rec("dead", R.dead.command("PING"));

R.slow = new Redis({ port: modern.port, host: "127.0.0.1",
                     commandTimeoutMs: 300 });
rec("slow", R.slow.command("SLOW"));

let subModeErr = null;
R.sub2 = new Redis({ port: legacy.port, host: "127.0.0.1" });
R.sub2.on("push", () => {});
rec("sub2", R.sub2.command("SUBSCRIBE", "x")
      .catch(() => "rejected")
      .then((v) => {
        try { R.sub2.command("GET", "k"); }
        catch (e) { subModeErr = String(e); }
        return v;
      }));

let tlsCtorErr = null;
const tlsProbe = new Redis({ port: 6379, tls: true, connectTimeoutMs: 100 });
tlsProbe.on("error", () => {});

let negPortErr = null;
try { new Redis({ port: -1, host: "127.0.0.1" }); }
catch (e) { negPortErr = String(e); }

let pendErr = null;
R.tiny = new Redis({ port: modern.port, host: "127.0.0.1", maxPending: 2 });
try {
  for (let i = 0; i < 40; i++) R.tiny.command("PING").catch(() => {});
} catch (e) { pendErr = String(e); }

function settled() {
  for (const k in results) if (results[k] === "pending") return false;
  return true;
}

let spins = 0;
const t = setInterval(() => {
  if (!settled() && spins++ < 800) return;
  clearInterval(t);

  const ok = (k) => results[k] && results[k].ok !== undefined;
  const val = (k) => results[k] && results[k].ok;
  const err = (k) => results[k] && results[k].err;
  const emsg = (k) => err(k) ? String(err(k).message || err(k)) : "";

  check(R.modern.protocol === 3,
        "HELLO 3 accepted must select protocol 3, got " + R.modern.protocol);
  check(R.modern.ready, "the client must reach READY");

  check(val("ping") === "PONG", "PING -> " + JSON.stringify(val("ping")));
  check(val("set") === "OK", "SET -> " + JSON.stringify(val("set")));
  check(val("get") === "v", "GET -> " + JSON.stringify(val("get")));
  check(val("miss") === null, "a missing key is null, got " + JSON.stringify(val("miss")));
  check(val("incr") === 42, "INCR -> " + val("incr"));
  check(val("dbl") === 3.5, "a RESP3 double -> " + val("dbl"));
  check(val("nil") === null, "RESP3 null -> " + JSON.stringify(val("nil")));
  check(val("true") === true, "RESP3 boolean -> " + val("true"));
  check(typeof val("bignum") === "string" && val("bignum").length === 23,
        "a big number stays TEXT (a double would lose digits), got " +
        JSON.stringify(val("bignum")));

  check(err("boom") && err("boom").code === "ERR",
        "an error reply must reject with code ERR, got " + emsg("boom"));
  check(err("boom") && /the command failed/.test(emsg("boom")),
        "and must carry the server's text: " + emsg("boom"));
  check(err("wrongtype") && err("wrongtype").code === "WRONGTYPE",
        "the error CLASS is the first token, got " +
        (err("wrongtype") && err("wrongtype").code));

  const lr = val("lrange");
  check(Array.isArray(lr) && lr.length === 2 && lr[0] === "a" && lr[1] === "b",
        "LRANGE -> " + JSON.stringify(lr));
  const mp = val("map");
  check(mp && typeof mp === "object" && mp.k === 9,
        "a RESP3 map becomes an object, got " + JSON.stringify(mp));

  const pp = val("pipe");
  check(Array.isArray(pp) && pp.length === 3, "pipeline length " +
        (pp && pp.length));
  check(pp && pp[0] === "PONG" && pp[1] === "OK" && pp[2] === "1",
        "pipeline order/values: " + JSON.stringify(pp));

  check(val("evilget") === EVIL,
        "a value containing a whole command must round-trip verbatim, got " +
        JSON.stringify(val("evilget")));
  const flush = modern.commands.filter((c) => c[0].toUpperCase() === "FLUSHALL");
  check(flush.length === 0,
        "the server must never PARSE a FLUSHALL out of an argument (saw " +
        flush.length + ")");
  const sets = modern.commands.filter((c) => c[0].toUpperCase() === "SET" &&
                                             c[1] === "evil");
  check(sets.length === 1 && sets[0].length === 3,
        "the hostile SET must arrive as exactly one 3-argument command, got " +
        JSON.stringify(sets));
  check(modern.parseError === undefined,
        "the mock must not have seen a malformed request: " + modern.parseError);

  const numc = modern.commands.filter((c) => c[1] === "num");
  check(numc.length === 1 && numc[0][2] === "12345",
        "a number argument coerces to its decimal text, got " +
        JSON.stringify(numc));
  const binc = modern.commands.filter((c) => c[1] === "bin");
  check(binc.length === 1 && binc[0][2] === "hi",
        "a Uint8Array argument goes out as its BYTES, got " +
        JSON.stringify(binc));

  const bg = val("binget");
  check(bg instanceof Uint8Array && bg.length === 1 && bg[0] === 118,
        "binary:true must give a Uint8Array, got " + Object.prototype.toString.call(bg));

  check(ok("sub"), "SUBSCRIBE must settle from its confirmation, not hang");
  check(pushes.length === 1, "exactly one unsolicited push, got " + pushes.length);
  check(pushes.length === 1 && pushes[0][0] === "message" &&
        pushes[0][2] === "hello-there",
        "the delivered message: " + JSON.stringify(pushes[0]));

  check(R.legacy.protocol === 2,
        "a server that refuses HELLO must downgrade to 2, got " + R.legacy.protocol);
  check(val("legacyping") === "PONG",
        "the downgraded client still works: " + JSON.stringify(val("legacyping")));
  const names = legacy.commands.map((c) => c[0].toUpperCase());
  const iAuth = names.indexOf("AUTH"), iSel = names.indexOf("SELECT"),
        iPing = names.indexOf("PING");
  check(iAuth >= 0 && iSel > iAuth && iPing > iSel,
        "AUTH then SELECT then the user's command, got " + JSON.stringify(names));

  check(err("badpass"), "a wrong password must reject the pending command");
  check(!/wrong/.test(emsg("badpass")),
        "the error must NOT contain the credential: " + emsg("badpass"));

  check(err("tls") && /TLS/.test(emsg("tls")),
        "a TLS record must be named, not reported as gibberish: " + emsg("tls"));
  check(err("garbage"), "an unknown type byte must reject: " + emsg("garbage"));
  check(err("huge"), "a 2e9-element array must reject: " + emsg("huge"));
  check(extraErr !== null && /no command outstanding/.test(extraErr),
        "a reply with no command outstanding must tear the connection down, " +
        "got " + extraErr);
  check(val("extra") === false,
        "and the client must not report itself ready afterwards, got " +
        val("extra"));
  check(err("deep"), "40 levels of nesting must reject: " + emsg("deep"));
  {
    let v = val("deepOk"), levels = 0;
    while (Array.isArray(v)) { v = v[0]; levels++; }
    check(levels === 31 && v === 7,
          "31 levels of nesting is under the cap and must parse through, got " +
          "levels=" + levels + " v=" + JSON.stringify(v));
    check(err("deep300") && /nesting/.test(emsg("deep300")),
          "300 levels of nesting must reject and name the cause: " +
          emsg("deep300"));
    check(err("attrchain"),
          "a 300-long attribute chain must refuse (decode recursion), got: " +
          emsg("attrchain"));
  }
  check(err("dead"), "a connect to a closed port must reject, not hang");
  check(err("slow") && /timed out/.test(emsg("slow")),
        "an unanswered command must hit its deadline: " + emsg("slow"));

  check(val("bigint") === "9007199254740993",
        "an integer past 2^53 must stay TEXT (a double rounds it), got " +
        JSON.stringify(val("bigint")));
  check(val("smallint") === 9007199254740992,
        "and one AT the boundary is still a number, got " +
        JSON.stringify(val("smallint")));
  check(val("bigIntOpt") === 9007199254740993n,
        "bigint:true must return the exact integer as a BigInt, got " +
        typeof val("bigIntOpt") + " " + String(val("bigIntOpt")));
  check(typeof val("smallIntOpt") === "bigint",
        "bigint:true applies to EVERY integer, not only the large ones -- a " +
        "type that changes with the value is worse than one that is wide, got " +
        typeof val("smallIntOpt"));
  check(val("info") === "Some string",
        "a verbatim string must drop its 3-byte display hint, got " +
        JSON.stringify(val("info")));
  check(val("attred") === "real",
        "an attribute decorates the next value and must not BE it, got " +
        JSON.stringify(val("attred")));
  const pk = val("protokey");
  check(pk && pk.__proto__ === 5 && Object.getPrototypeOf(pk) !== null,
        "a '__proto__' key must be defined, not retarget the prototype, got " +
        JSON.stringify(pk) + " proto=" + Object.getPrototypeOf(pk));
  const bm = val("binmap");
  check(bm && bm.maxmemory instanceof Uint8Array,
        "under binary the map VALUE is bytes, got " + JSON.stringify(bm));
  check(bm && Object.keys(bm)[0] === "maxmemory",
        "but the KEY stays text -- a Uint8Array key would stringify to " +
        "'109,97,...', got " + (bm && Object.keys(bm)[0]));

  check(ok("multisub"), "a 3-channel SUBSCRIBE must settle");
  const ms = val("multisub");
  check(Array.isArray(ms) && ms.length === 3,
        "and must collect all THREE confirmations, got " + JSON.stringify(ms));
  check(val("aftersub") === "PONG",
        "the command AFTER it must get its own reply, not confirmation #2 -- " +
        "got " + JSON.stringify(val("aftersub")));
  check(multiErr !== null && /unsubscribe by name/.test(multiErr),
        "an argument-less UNSUBSCRIBE must be refused, not guessed: " + multiErr);
  check(subModeErr !== null && /not allowed while subscribed/.test(subModeErr),
        "a negative port must be refused, not wrapped into range: " + negPortErr);
  check(pendErr !== null && /maxPending|in flight/.test(pendErr),
        "maxPending must be enforced, got " + pendErr);

  for (const k in R) R[k].close();
  modern.close(); legacy.close();
  srvTls.close(); srvGarbage.close(); srvHugeCnt.close();
  srvExtra.close(); srvDeepNest.close();
  srvDeepOk.close(); srvDeep300.close(); srvAttrChain.close();

  if (fails === 0) print("test_net_redis: all " + n + " checks passed");
  else print("test_net_redis: " + fails + " FAILED of " + n);
}, 10);
