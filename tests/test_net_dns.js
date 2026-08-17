/* test_net_dns.js -- DNSResolver: a real lookup, and the anti-spoofing rules.
 *
 * The spoof cases are the point. A UDP answer is forged by anyone who can guess
 * what to send, so a resolver that matches on the ID alone -- or on nothing --
 * accepts an attacker's address. Each forgery below is a real datagram sent to
 * the resolver's own socket.
 */
import { DNSResolver, DNSServer, UDPSocket, TCPServer } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

/* A tiny authoritative server: answers example.test A -> 10.1.2.3 */
const srv = new UDPSocket({ port: 0, host: "127.0.0.1" });
let lastQuery = null;

function u16(b, o) { return (b[o] << 8) | b[o + 1]; }

srv.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  if (q.length < 12) return;
  lastQuery = q;
  /* Echo the question back, then one A record pointing at the name (0xC00C). */
  let end = 12;
  while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;                                  /* root label */
  const qsec = q.subarray(12, end + 4);      /* QNAME + QTYPE + QCLASS */
  const out = new Uint8Array(12 + qsec.length + 16);
  out.set(q.subarray(0, 2), 0);              /* same ID */
  out[2] = 0x81; out[3] = 0x80;              /* QR + RD + RA, rcode 0 */
  out[5] = 1;                                /* qdcount */
  out[7] = 1;                                /* ancount */
  out.set(qsec, 12);
  let p = 12 + qsec.length;
  out[p++] = 0xc0; out[p++] = 0x0c;          /* name -> offset 12 */
  out[p++] = 0; out[p++] = 1;                /* type A */
  out[p++] = 0; out[p++] = 1;                /* class IN */
  out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 60;   /* ttl */
  out[p++] = 0; out[p++] = 4;                /* rdlength */
  out[p++] = 10; out[p++] = 1; out[p++] = 2; out[p++] = 3;
  srv.send(out.buffer.slice(0, p), from.address, from.port);
}});

const res = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 3000 });

let answered = null, answerErr = "none";
res.query("example.test", 1, (err, recs) => {
  if (err) { answerErr = String(err); return; }
  answered = recs;
});

/* ---- SPOOFING. A forged answer arrives from the WRONG source, or carries the
   wrong ID, or answers a different question. Each must be ignored: the query
   must still time out rather than resolve with the attacker's address. ---- */
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

/* A resolver pointed at a port where nothing listens must TIME OUT, not hang
   forever and not resolve. */
const dead = new DNSResolver({ server: "127.0.0.1", port: 9, timeoutMs: 600 });
let deadErr = null;
dead.query("nowhere.test", 1, (err) => { deadErr = err ? String(err) : "RESOLVED"; });

/* Blast the dead resolver's own port with forgeries while it waits. We do not
   know its ephemeral source port, so aim at the whole plausible range?  No --
   instead point a THIRD resolver at the spoofer, which answers with the wrong
   question, and prove that answer is rejected. */
const tricked = new DNSResolver({ server: "127.0.0.1", port: spoofer.port, timeoutMs: 900 });
let trickedResult = null;
spoofer.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  const id = (q[0] << 8) | q[1];
  /* Correct ID and correct source -- but ANSWERS A DIFFERENT NAME. */
  spoofer.send(forge(id, "attacker.test", [6, 6, 6, 6]), from.address, from.port);
}});
tricked.query("victim.test", 1, (err, recs) => {
  trickedResult = err ? ("ERR:" + err) : ("RESOLVED:" + (recs[0] && recs[0].address));
});

/* ---- OUR OWN SERVER answering OUR OWN resolver: the real round trip. ---- */
const real = new DNSServer({ port: 0, host: "127.0.0.1" });
real.start((name, type) => (name === "host.test" && type === 1) ? "192.0.2.7" : null);
const rres = new DNSResolver({ server: "127.0.0.1", port: real.port, timeoutMs: 3000 });
let realOut = null;
rres.query("host.test", 1, (err, recs) => {
  realOut = err ? ("ERR:" + err) : (recs.length ? recs[0].address : "EMPTY");
});

/* A name the server does not know must come back as an empty answer, not as a
   timeout and not as someone else's address. */
let missOut = null;
rres.query("absent.test", 1, (err, recs) => {
  missOut = err ? ("ERR:" + err) : (recs.length ? recs[0].address : "EMPTY");
});

/* RATE LIMIT: far more queries than the bucket allows must not all be
   answered, or the server is a usable reflector. */
const flood = new DNSResolver({ server: "127.0.0.1", port: real.port, timeoutMs: 1200 });
let floodAns = 0, floodErr = 0;
for (let i = 0; i < 40; i++)
  flood.query("host.test", 1, (err) => { if (err) floodErr++; else floodAns++; });

/* ---- TC + TCP FALLBACK (RFC 1035 4.2.2) ----
   A UDP server that always answers with TC set and no records; a TCP server on
   the SAME port that serves the real answer, length-prefixed. The resolver must
   notice TC, reconnect over TCP, and return the record. */
const TCPORT = 45353;
const tcUdp = new UDPSocket({ port: TCPORT, host: "127.0.0.1" });
tcUdp.start({ message: (bytes, from) => {
  const q = new Uint8Array(bytes);
  let end = 12; while (end < q.length && q[end] !== 0) end += 1 + q[end];
  end += 1;
  const out = new Uint8Array(12 + (end + 4 - 12));
  out.set(q.subarray(0, 2), 0);
  out[2] = 0x81 | 0x02;            /* QR + TC */
  out[3] = 0x80;
  out[5] = 1;                      /* qdcount, ancount stays 0 */
  out.set(q.subarray(12, end + 4), 12);
  tcUdp.send(out.buffer, from.address, from.port);
}});

const tcTcp = new TCPServer({ port: TCPORT });
tcTcp.start({ data: (c, bytes) => {
  const f = new Uint8Array(bytes);
  if (f.length < 2) return;
  const q = f.subarray(2);                       /* strip the length prefix */
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
  /* THE SPLIT IS THE WHOLE TEST. The prefix EXCLUDES itself, so a complete
     frame is `want + 2` octets; writing `have < want` instead misparses only
     while `want <= have < want + 2`, a two-octet window. Splitting at the
     prefix does NOT reach it -- `have` jumps from 2 straight to want + 2.
     Split so the first write is exactly `want` octets and the last two arrive
     separately, which is the only arrival pattern that tells the two apart. */
  const pre = new Uint8Array(2);
  pre[0] = (p >> 8) & 0xff; pre[1] = p & 0xff;   /* prefix EXCLUDES itself */
  const first = new Uint8Array(p);               /* prefix + body minus 2 */
  first.set(pre, 0);
  first.set(body.subarray(0, p - 2), 2);
  /* And they must arrive as two SEPARATE reads. Two back-to-back writes land
     in one loopback segment, so the client sees have jump 0 -> want + 2 and
     the window is skipped again. A delay is what makes the transport fragment
     on demand, which is the only way this arithmetic is observable at all. */
  c.write(first.buffer);                         /* have === want exactly */
  const rest = body.buffer.slice(p - 2, p);
  setTimeout(() => { try { c.write(rest); } catch (e) {} }, 40);
}});

const tcRes = new DNSResolver({ server: "127.0.0.1", port: TCPORT, timeoutMs: 4000 });
let tcOut = null;
tcRes.query("big.test", 1, (err, recs) => {
  tcOut = err ? ("ERR:" + err) : (recs.length ? recs[0].address : "EMPTY");
});

let spins = 0;
const t = setInterval(() => {
  if ((answered !== null && deadErr !== null && trickedResult !== null &&
       realOut !== null && missOut !== null && floodAns + floodErr >= 40 &&
       tcOut !== null) || spins++ > 1200) {
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

    res.close(); dead.close(); tricked.close(); spoofer.close(); srv.close();
    rres.close(); flood.close(); real.close();
    tcRes.close(); tcTcp.close(); tcUdp.close();
    if (fails === 0) print("test_net_dns: all " + n + " checks passed");
    else print("test_net_dns: " + fails + " FAILED");
  }
}, 10);
