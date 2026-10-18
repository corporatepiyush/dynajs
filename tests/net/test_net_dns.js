import { DNSResolver, DNSServer, UDPSocket, TCPServer } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

{
    function throwsMsgC6(fn, re, m) {
        let threw = null;
        try { fn(); } catch (e) { threw = e; }
        check(threw !== null && threw instanceof TypeError && re.test(threw.message),
              m + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    }

    throwsMsgC6(() => new DNSResolver({ server: "127.0.0.1", timeoutz: 5 }),
                /unknown option "timeoutz" \(valid: server, port, timeoutMs, ttl\)/,
                "DNSResolver bag");
    throwsMsgC6(() => new DNSServer({ hostt: "127.0.0.1" }),
                /unknown option "hostt" \(valid: port, host\)/,
                "DNSServer bag");
}

const srv = new UDPSocket({ port: 0, host: "127.0.0.1" });
let lastQuery = null;

function u16(b, o) { return (b[o] << 8) | b[o + 1]; }

srv.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  if (q.length < 12) return;
  lastQuery = q;
  let end = 12;
  while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;
  const qsec = q.subarray(12, end + 4);
  const out = new Uint8Array(12 + qsec.length + 16);
  out.set(q.subarray(0, 2), 0);
  out[2] = 0x81; out[3] = 0x80;
  out[5] = 1;
  out[7] = 1;
  out.set(qsec, 12);
  let p = 12 + qsec.length;
  out[p++] = 0xc0; out[p++] = 0x0c;
  out[p++] = 0; out[p++] = 1;
  out[p++] = 0; out[p++] = 1;
  out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 60;
  out[p++] = 0; out[p++] = 4;
  out[p++] = 10; out[p++] = 1; out[p++] = 2; out[p++] = 3;
  srv.send(out.buffer.slice(0, p), from.address, from.port);
}});

const res = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 3000 });

{
  let threw = null;
  try { res.query("example.test", 99999, () => {}); } catch (e) { threw = e; }
  check(threw && String(threw).includes("type out of range"),
        "query type 99999 must throw a range error, got " + threw);
  threw = null;
  try { res.query("example.test", 0, () => {}); } catch (e) { threw = e; }
  check(threw && String(threw).includes("type out of range"),
        "query type 0 must throw too, got " + threw);
}

let answered = null, answerErr = "none";
res.query("example.test", 1, (err, recs) => {
  if (err) { answerErr = String(err); return; }
  answered = recs;
});

const spoofer = new UDPSocket({ port: 0, host: "127.0.0.1" });
function forge(id, qname, addr) {
  const labels = qname.split(".");
  let qlen = 1; for (const l of labels) qlen += 1 + l.length;
  const out = new Uint8Array(12 + qlen + 4 + 16);
  out[0] = id >> 8; out[1] = id & 0xff;
  out[2] = 0x81; out[3] = 0x80; out[5] = 1; out[7] = 1;
  let p = 12;
  for (const l of labels) { out[p++] = l.length; for (const ch of l) out[p++] = ch.charCodeAt(0); }
  out[p++] = 0;
  out[p++] = 0; out[p++] = 1; out[p++] = 0; out[p++] = 1;
  out[p++] = 0xc0; out[p++] = 0x0c;
  out[p++] = 0; out[p++] = 1; out[p++] = 0; out[p++] = 1;
  out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 60;
  out[p++] = 0; out[p++] = 4;
  for (const o of addr) out[p++] = o;
  return out.buffer.slice(0, p);
}

const dead = new DNSResolver({ server: "127.0.0.1", port: 9, timeoutMs: 600 });
let deadErr = null;
dead.query("nowhere.test", 1, (err) => { deadErr = err ? String(err) : "RESOLVED"; });

const tricked = new DNSResolver({ server: "127.0.0.1", port: spoofer.port, timeoutMs: 900 });
let trickedResult = null;
spoofer.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  const id = (q[0] << 8) | q[1];
  spoofer.send(forge(id, "attacker.test", [6, 6, 6, 6]), from.address, from.port);
}});
tricked.query("victim.test", 1, (err, recs) => {
  trickedResult = err ? ("ERR:" + err) : ("RESOLVED:" + (recs[0] && recs[0].address));
});

const real = new DNSServer({ port: 0, host: "127.0.0.1" });
real.start((name, type) => (name === "host.test" && type === 1) ? "192.0.2.7" : null);
const rres = new DNSResolver({ server: "127.0.0.1", port: real.port, timeoutMs: 3000 });
let realOut = null;
rres.query("host.test", 1, (err, recs) => {
  realOut = err ? ("ERR:" + err) : (recs.length ? recs[0].address : "EMPTY");
});

let missOut = null;
rres.query("absent.test", 1, (err, recs) => {
  missOut = err ? ("ERR:" + err) : (recs.length ? recs[0].address : "EMPTY");
});

const flood = new DNSResolver({ server: "127.0.0.1", port: real.port, timeoutMs: 1200 });
let floodAns = 0, floodErr = 0;
for (let i = 0; i < 40; i++)
  flood.query("host.test", 1, (err) => { if (err) floodErr++; else floodAns++; });

const TCPORT = 45353;
const tcUdp = new UDPSocket({ port: TCPORT, host: "127.0.0.1" });
tcUdp.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  let end = 12; while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;
  const out = new Uint8Array(12 + (end + 4 - 12));
  out.set(q.subarray(0, 2), 0);
  out[2] = 0x81 | 0x02;
  out[3] = 0x80;
  out[5] = 1;
  out.set(q.subarray(12, end + 4), 12);
  tcUdp.send(out.buffer, from.address, from.port);
}});

const tcTcp = new TCPServer({ port: TCPORT });
tcTcp.start({ data: (c, bytes) => {
  const f = new Uint8Array(bytes);
  if (f.length < 2) return;
  const q = f.subarray(2);
  let end = 12; while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;
  const qsec = q.subarray(12, end + 4);
  const body = new Uint8Array(12 + qsec.length + 16);
  body.set(q.subarray(0, 2), 0);
  body[2] = 0x81; body[3] = 0x80; body[5] = 1; body[7] = 1;
  body.set(qsec, 12);
  let p = 12 + qsec.length;
  body[p++] = 0xc0; body[p++] = 0x0c;
  body[p++] = 0; body[p++] = 1; body[p++] = 0; body[p++] = 1;
  body[p++] = 0; body[p++] = 0; body[p++] = 0; body[p++] = 30;
  body[p++] = 0; body[p++] = 4;
  body[p++] = 172; body[p++] = 16; body[p++] = 0; body[p++] = 9;
  const pre = new Uint8Array(2);
  pre[0] = (p >> 8) & 0xff; pre[1] = p & 0xff;
  const first = new Uint8Array(p);
  first.set(pre, 0);
  first.set(body.subarray(0, p - 2), 2);
  c.write(first.buffer);
  const rest = body.buffer.slice(p - 2, p);
  setTimeout(() => { try { c.write(rest); } catch (e) {} }, 40);
}});

const tcRes = new DNSResolver({ server: "127.0.0.1", port: TCPORT, timeoutMs: 4000 });
let tcOut = null;
tcRes.query("big.test", 1, (err, recs) => {
  tcOut = err ? ("ERR:" + err) : (recs.length ? recs[0].address : "EMPTY");
});


const hostile = new UDPSocket({ port: 0, host: "127.0.0.1" });
let hMode = 0;
hostile.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  const id = (q[0] << 8) | q[1];
  let end = 12; while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;
  const qsec = q.subarray(12, end + 4);
  const mode = hMode++ % 3;
  let out;
  if (mode === 0) {
    out = new Uint8Array(12);
    out[0] = id >> 8; out[1] = id & 0xff;
    out[2] = 0x81; out[3] = 0x80;
  } else if (mode === 1) {
    out = new Uint8Array(12 + 4);
    out[0] = id >> 8; out[1] = id & 0xff;
    out[2] = 0x81; out[3] = 0x80; out[5] = 1; out[7] = 1;
    out[12] = 0xc0; out[13] = 0x0c;
    out[14] = 0; out[15] = 1;
  } else {
    out = new Uint8Array(12 + qsec.length + 2 + 10 + 2);
    out[0] = id >> 8; out[1] = id & 0xff;
    out[2] = 0x81; out[3] = 0x80; out[5] = 1; out[7] = 1;
    out.set(qsec, 12);
    let p = 12 + qsec.length;
    out[p++] = 0xc0; out[p++] = 0x0c;
    out[p++] = 0; out[p++] = 1; out[p++] = 0; out[p++] = 1;
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 60;
    out[p++] = 0xff; out[p++] = 0xff;
    out[p++] = 1; out[p++] = 2;
    out = out.subarray(0, p);
  }
  hostile.send(out.buffer ? out.buffer.slice(out.byteOffset, out.byteOffset + out.length) : out,
               from.address, from.port);
}});
const hres = new DNSResolver({ server: "127.0.0.1", port: hostile.port, timeoutMs: 500 });
const hOut = [null, null, null];
for (let i = 0; i < 3; i++) {
  ((i) => hres.query("hostile" + i + ".test", 1, (err, recs) => {
    hOut[i] = err ? "ERR" : (recs && recs.length === 0 ? "EMPTY" : "REC");
  }))(i);
}

const nta = new UDPSocket({ port: 0, host: "127.0.0.1" });
let ntaQueries = 0, ntaMixed = false, ntaTtl = 60;

function pushName(a, name) {
  for (const l of name.split(".")) {
    a.push(l.length);
    for (const ch of l) a.push(ch.charCodeAt(0));
  }
  a.push(0);
}
function pushRR(a, name, type, ttl, rdata) {
  pushName(a, name);
  a.push(type >> 8, type & 0xff, 0, 1);
  a.push((ttl >>> 24) & 0xff, (ttl >> 16) & 0xff, (ttl >> 8) & 0xff, ttl & 0xff);
  a.push((rdata.length >> 8) & 0xff, rdata.length & 0xff);
  for (const b of rdata) a.push(b);
}
function nameRdata(name) { const a = []; pushName(a, name); return a; }

nta.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  if (q.length < 12) return;
  ntaQueries++;
  const id = (q[0] << 8) | q[1];
  let end = 12;
  while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;
  const qsec = q.subarray(12, end + 4);
  const qtype = (q[end] << 8) | q[end + 1];
  let at = 12, labels = [];
  while (q[at] !== 0) {
    const l = q[at++];
    let s = "";
    for (let i = 0; i < l; i++) s += String.fromCharCode(q[at + i]);
    labels.push(s);
    at += l;
  }
  const owner = labels.join(".");
  const answers = [];
  const ttl = ntaTtl;
  if (qtype === 1) {
    answers.push([owner, 1, ttl, [10, 9, 8, 7]]);
    if (ntaMixed)
      answers.push([owner, 5, ttl, nameRdata("cnameweight.test")]);
  } else if (qtype === 5) {
    answers.push([owner, 5, ttl, nameRdata("target.test")]);
  } else if (qtype === 15) {
    answers.push([owner, 15, ttl, [0, 10].concat(nameRdata("mx.test"))]);
  } else if (qtype === 16) {
    answers.push([owner, 16, ttl, [5].concat("alpha".split("").map(c => c.charCodeAt(0)))
                                    .concat([4]).concat("beta".split("").map(c => c.charCodeAt(0)))]);
  } else if (qtype === 2) {
    answers.push([owner, 2, ttl, nameRdata("ns1.test")]);
  }
  const a = [id >> 8, id & 0xff, 0x81, 0x80, 0, 1, 0, answers.length, 0, 0, 0, 0];
  for (const b of qsec) a.push(b);
  for (const [on, t, tl, rd] of answers) pushRR(a, on, t, tl, rd);
  nta.send(new Uint8Array(a).buffer.slice(0, a.length), from.address, from.port);
}});

let nt1Pending = 11;
function nt1Done() { nt1Pending--; }
const ntRes = new DNSResolver({ server: "127.0.0.1", port: nta.port, timeoutMs: 3000 });
const ntCache = new DNSResolver({ server: "127.0.0.1", port: nta.port, timeoutMs: 3000, ttl: 60 });

ntRes.lookup("nt1a.test").then((recs) => {
  check(recs.length === 1 && recs[0].address === "10.9.8.7" &&
        recs[0].name === "nt1a.test" && recs[0].type === 1 && recs[0].ttl === 60,
        "lookup promise form resolves DNSRecord[] (" + JSON.stringify(recs) + ")");
  nt1Done();
}, (e) => { check(false, "lookup rejected: " + e); nt1Done(); });

ntRes.lookup("nt1t.test", 16).then((recs) => {
  check(recs.length === 1 && recs[0].type === 16,
        "lookup(name, TXT) decodes the TXT record");
  nt1Done();
}, (e) => { check(false, "lookup(name,16) rejected: " + e); nt1Done(); });

const deadLk = new DNSResolver({ server: "127.0.0.1", port: 9, timeoutMs: 400 });
deadLk.lookup("nowhere.test").then(() => {
  check(false, "lookup against a dead server must reject");
  nt1Done();
}, (e) => {
  check(e instanceof Error && /timed out/.test(e.message),
        "a failed lookup rejects with an Error naming the failure (" + e + ")");
  nt1Done();
});

let cbOut = null;
ntRes.lookup("nt1cb.test", (err, recs) => { cbOut = err ? "ERR" : recs[0].address; nt1Done(); });
let cb2Out = null;
ntRes.lookup("nt1cb2.test", 1, (err, recs) => { cb2Out = err ? "ERR" : recs[0].address; nt1Done(); });

ntRes.resolveCname("alias.test").then((xs) => {
  check(xs.length === 1 && xs[0] === "target.test",
        "resolveCname returns the targets (" + JSON.stringify(xs) + ")");
  nt1Done();
}, (e) => { check(false, "resolveCname rejected: " + e); nt1Done(); });
ntRes.resolveMx("mail.test").then((xs) => {
  check(xs.length === 1 && xs[0].priority === 10 && xs[0].exchange === "mx.test",
        "resolveMx returns {priority, exchange} (" + JSON.stringify(xs) + ")");
  nt1Done();
}, (e) => { check(false, "resolveMx rejected: " + e); nt1Done(); });
ntRes.resolveTxt("txt.test").then((xs) => {
  check(xs.length === 1 && xs[0].length === 2 && xs[0][0] === "alpha" && xs[0][1] === "beta",
        "resolveTxt keeps the character-strings apart (" + JSON.stringify(xs) + ")");
  nt1Done();
}, (e) => { check(false, "resolveTxt rejected: " + e); nt1Done(); });
ntRes.resolveNs("zone.test").then((xs) => {
  check(xs.length === 1 && xs[0] === "ns1.test",
        "resolveNs returns the nameservers (" + JSON.stringify(xs) + ")");
  nt1Done();
}, (e) => { check(false, "resolveNs rejected: " + e); nt1Done(); });

ntaMixed = true;
ntRes.lookup("poison.test").then((recs) => {
  check(recs.length === 1 && recs[0].type === 1,
        "a same-owner CNAME smuggled into an A answer is filtered out (" +
        JSON.stringify(recs) + ")");
  nt1Done();
}, (e) => { check(false, "poison lookup rejected: " + e); nt1Done(); });

let cachePhase = 0, cacheFirst = null;
const cacheTests = [];
function cacheStep(fn) { cacheTests.push(fn); }
function cacheNext() { if (cachePhase < cacheTests.length) cacheTests[cachePhase++](); }

cacheStep(() => setTimeout(cacheNext, 60));
cacheStep(() => { ntaTtl = 60; ntaMixed = false; ntaQueries = 0;
  ntCache.lookup("cached.test").then((recs) => {
    cacheFirst = recs;
    check(ntaQueries === 1, "a cache miss goes to the wire (queries=" + ntaQueries + ")");
    cacheNext();
  });
});
cacheStep(() => {
  ntCache.lookup("cached.test").then((recs) => {
    check(ntaQueries === 1, "a fresh answer is served from the cache, no wire query");
    check(recs[0].address === cacheFirst[0].address, "the cached answer is the answer");
    cacheNext();
  });
});
cacheStep(() => {
  ntCache.lookup("CACHED.test").then(() => {
    check(ntaQueries === 1,
          "the cache key is case-insensitive (RFC 1035 2.3.3), no wire query");
    cacheNext();
  });
});
cacheStep(() => {
  ntCache.lookup("other.test").then(() => {
    check(ntaQueries === 2, "a different name is a different cache key (queries=" + ntaQueries + ")");
    cacheNext();
  });
});
cacheStep(() => {
  ntaTtl = 0;
  ntCache.lookup("ttlzero.test").then(() => cacheNext());
});
cacheStep(() => {
  ntCache.lookup("ttlzero.test").then(() => {
    check(ntaQueries === 4,
          "a TTL-0 answer is never cached (queries=" + ntaQueries + ", want 4)");
    ntaTtl = 60;
    cacheNext();
  });
});
cacheStep(() => { ntCache.lookup("poisonme.test").then(() => cacheNext()); });
cacheStep(() => { ntCache.lookup("poisonme.test").then(() => cacheNext()); });
cacheStep(() => {
  ntCache.lookup("poisonme.test").then((recs) => {
    recs[0].address = "6.6.6.6";
    recs.push({ name: "junk", type: 0, ttl: 0, address: "9.9.9.9" });
    cacheNext();
  });
});
cacheStep(() => {
  ntCache.lookup("poisonme.test").then((recs) => {
    check(recs.length === 1 && recs[0].address === "10.9.8.7",
          "a mutated hit cannot poison the cache (got " +
          JSON.stringify(recs) + ")");
    cacheNext();
  });
});
cacheStep(() => {
  ntRes.lookup("nocache1.test").then(() => cacheNext());
});
cacheStep(() => {
  ntRes.lookup("nocache1.test").then(() => {
    check(ntaQueries === 7,
          "caching is opt-in: no {ttl}, no cache (queries=" + ntaQueries + ", want 7)");
    nt1Done();
    cacheNext();
  });
});

cacheNext();

let spins = 0;
const t = setInterval(() => {
    if ((answered !== null && deadErr !== null && trickedResult !== null &&
       realOut !== null && missOut !== null && floodAns + floodErr >= 40 &&
       tcOut !== null && hOut[0] !== null && hOut[1] !== null && hOut[2] !== null &&
       nt1Pending === 0) ||
       spins++ > 2400) {
    clearInterval(t);

    check(answered !== null, "a legitimate answer must resolve (err=" + answerErr + ")");
    if (answered) {
      check(answered.length === 1, "got " + answered.length + " records, want 1");
      check(answered[0] && answered[0].address === "10.1.2.3",
            "address '" + (answered[0] && answered[0].address) + "', want 10.1.2.3");
      check(answered[0].name === "example.test",
            "the compressed name must decode to example.test, got '" +
            answered[0].name + "'");
      check(answered[0].ttl === 60, "ttl " + answered[0].ttl);
    }
    check(deadErr !== null && deadErr !== "RESOLVED",
          "a query to a dead server must time out, got " + deadErr);
    check(realOut === "192.0.2.7",
          "our DNSServer must answer our DNSResolver, got " + realOut);
    check(missOut === "EMPTY",
          "an unknown name must return no records, not an error, got " + missOut);
    check(floodErr > 0,
          "the per-source rate limit must drop some of 40 rapid queries " +
          "(answered=" + floodAns + " dropped=" + floodErr + ") -- otherwise " +
          "the server is a usable reflection amplifier");
    check(tcOut === "172.16.0.9",
          "a TC response must trigger the TCP fallback and return the real " +
          "answer, got " + tcOut);
    check(trickedResult !== null && trickedResult.startsWith("ERR:"),
          "an answer for a DIFFERENT question must be rejected even with the " +
          "right ID and source -- got " + trickedResult);

    check(hOut[0] === "ERR" && hOut[1] === "ERR",
          "qdcount=0 and a self-pointing question pointer must never settle (" +
          hOut + ")");
    check(hOut[2] === "EMPTY",
          "an RDLENGTH past the datagram settles empty, not an error (" + hOut[2] + ")");
    check(nt1Pending === 0,
          "every NT-1 promise/cache assertion settled (" + nt1Pending + " left)");
    res.close(); dead.close(); tricked.close(); spoofer.close(); srv.close();
    hres.close(); hostile.close();
    rres.close(); flood.close(); real.close();
    tcRes.close(); tcTcp.close(); tcUdp.close();
    nta.close(); ntRes.close(); ntCache.close(); deadLk.close();
    if (fails === 0) print("test_net_dns: all " + n + " checks passed");
    else print("test_net_dns: " + fails + " FAILED");
  }
}, 10);
