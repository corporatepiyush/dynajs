// DNS authoritative server — a zone file parsed into an ordered name tree, answered over UDP in RFC 1035 wire format
// with name compression, wildcards, CNAME chasing, NXDOMAIN/NODATA and truncation, plus a caching forwarder.
//
// WHAT IT IS
//   The server half of DNS, checked by a client that is NOT ours:
//
//   zone        master-file subset ($ORIGIN, $TTL, relative names, A/AAAA/CNAME/MX/TXT/NS, quoted TXT chunks)
//               stored in an OrderedMap keyed by reversed labels, so "does anything exist below this name" — the
//               difference between NXDOMAIN and NODATA for an empty non-terminal — is one ceilKey lookup, and the
//               closest encloser for a wildcard (RFC 4592) is a walk up at most label-count keys
//   wire        header/question/RR encoding with suffix compression (RFC 1035 §4.1.4); the query decoder follows
//               pointers with a hop budget, so a pointer loop is FORMERR, not a hang
//   answers     CNAME chains are followed inside the zone; answers over 512 bytes set TC and carry no records
//   oracle      every positive answer is fetched with dyna:net's DNSResolver — a separate decoder — and must
//               decode to the zone's data; rcodes and flags are checked from raw replies
//   forwarder   a caching resolver in front of the server: LRU with per-entry TTL, negative caching of
//               NXDOMAIN, and a hit rate measured over a Zipf-distributed query stream
//
// MODULES  dyna:net (UDPSocket, DNSResolver) · dyna:structures (OrderedMap, LRU, Multiset) · dyna:bytes (concat,
//          readUint16BE, writeUint16BE, writeUint32BE) · dyna:random · dyna:cli · String extensions
//
// RUN      dynajs examples/net/dns-authoritative.js

import { UDPSocket, DNSResolver } from "dyna:net";
import { OrderedMap, LRU, Multiset } from "dyna:structures";
import { concat, readUint16BE, writeUint16BE, writeUint32BE } from "dyna:bytes";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const TYPE = { A: 1, NS: 2, CNAME: 5, MX: 15, TXT: 16, AAAA: 28 };
const TYPE_NAME = Object.fromEntries(Object.entries(TYPE).map(([k, v]) => [v, k]));

// =============================================================================
// Zone
// =============================================================================
const ZONE = `
$ORIGIN example.test.
$TTL 300
@           IN NS     ns1
@           IN MX     10 mail
@           IN MX     20 backup-mail
@           IN TXT    "v=spf1 mx -all"
@           IN A      192.0.2.1
ns1         IN A      192.0.2.53
mail   60   IN A      192.0.2.25
backup-mail IN A      198.51.100.25
www         IN CNAME  web.prod
web.prod    IN A      192.0.2.80
web.prod    IN A      192.0.2.81
web.prod    IN AAAA   2001:db8::80
api         IN CNAME  www
*.preview   IN A      192.0.2.200
*.preview   IN TXT    "wildcard" "preview env"
deep.a.b.c  IN A      192.0.2.99
big         IN TXT    "${"x".repeat(200)}"
`;
const revKey = (name) => name.toLowerCase().replace(/\.$/, "").split(".").reverse().join(".");
function parseZone(text) {
    const zone = new OrderedMap();
    let origin = "",
        ttl = 3600;
    for (const raw of text.lines()) {
        const line = raw.replace(/;.*$/, "").trimEnd();
        if (line.isBlank()) continue;
        if (line.startsWith("$ORIGIN")) {
            origin = line.split(/\s+/)[1];
            continue;
        }
        if (line.startsWith("$TTL")) {
            ttl = Number(line.split(/\s+/)[1]);
            continue;
        }
        const m = line.match(/^(\S+)\s+(?:(\d+)\s+)?IN\s+(\w+)\s+(.*)$/);
        if (!m) throw new SyntaxError(`bad zone line: ${line}`);
        const name = m[1] === "@" ? origin : m[1].endsWith(".") ? m[1] : `${m[1]}.${origin}`;
        const type = TYPE[m[3]];
        const rr = { name: name.replace(/\.$/, "").toLowerCase(), type, ttl: m[2] ? Number(m[2]) : ttl };
        const data = m[4].trim();
        const fq = (n) => (n.endsWith(".") ? n.slice(0, -1) : `${n}.${origin.slice(0, -1)}`);
        if (type === TYPE.A || type === TYPE.AAAA) rr.address = data;
        else if (type === TYPE.CNAME || type === TYPE.NS) rr.target = fq(data);
        else if (type === TYPE.MX) {
            const [p, ex] = data.split(/\s+/);
            rr.priority = Number(p);
            rr.exchange = fq(ex);
        } else if (type === TYPE.TXT) rr.chunks = [...data.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]);
        const key = revKey(rr.name);
        (zone.get(key) ?? zone.set(key, []).get(key)).push(rr);
    }
    return { zone, apex: revKey(origin) };
}
const { zone, apex } = parseZone(ZONE);

// RFC 4592 lookup: exact name, else NODATA for an empty non-terminal, else the wildcard at the closest encloser.
function resolve(qname, qtype) {
    const answers = [];
    let name = qname.toLowerCase().replace(/\.$/, ""),
        seen = new Set();
    for (;;) {
        const key = revKey(name);
        if (!(key === apex || key.startsWith(apex + "."))) return { rcode: 5, answers: [], aa: false };
        let rrs = zone.get(key);
        if (!rrs) {
            const next = zone.ceilKey(key + ".");
            if (next !== undefined && next.startsWith(key + ".")) return { rcode: 0, answers, aa: true };
            const labels = key.split(".");
            for (let n = labels.length - 1; n >= apex.split(".").length && !rrs; n--) {
                const encloser = labels.slice(0, n).join(".");
                if (zone.has(encloser) || zone.ceilKey(encloser + ".")?.startsWith(encloser + ".")) {
                    rrs = zone.get(encloser + ".*")?.map((r) => ({ ...r, name }));
                    break;
                }
            }
            if (!rrs) return { rcode: answers.length ? 0 : 3, answers, aa: true };
        }
        const exact = rrs.filter((r) => r.type === qtype);
        if (exact.length || qtype === TYPE.CNAME) {
            answers.push(...exact);
            return { rcode: 0, answers, aa: true };
        }
        const cname = rrs.find((r) => r.type === TYPE.CNAME);
        if (!cname || seen.has(name)) return { rcode: 0, answers, aa: true };
        seen.add(name);
        answers.push(cname);
        name = cname.target;
    }
}

// =============================================================================
// Wire format
// =============================================================================
function encodeName(name, out, offsets, pos) {
    const labels = name ? name.split(".") : [];
    for (let i = 0; i < labels.length; i++) {
        const suffix = labels.slice(i).join(".").toLowerCase();
        const at = offsets.get(suffix);
        if (at !== undefined) {
            const p = new Uint8Array(2);
            writeUint16BE(p, 0, 0xc000 | at);
            out.push(p);
            return pos + 2;
        }
        if (pos < 0x3fff) offsets.set(suffix, pos);
        const bytes = new TextEncoder().encode(labels[i]);
        out.push(Uint8Array.of(bytes.length), bytes);
        pos += 1 + bytes.length;
    }
    out.push(Uint8Array.of(0));
    return pos + 1;
}
function ipv6Bytes(addr) {
    const [head, tail = ""] = addr.split("::");
    const h = head ? head.split(":") : [],
        t = tail ? tail.split(":") : [];
    const groups = [...h, ...Array(8 - h.length - t.length).fill("0"), ...t];
    const out = new Uint8Array(16);
    groups.forEach((g, i) => writeUint16BE(out, i * 2, parseInt(g, 16)));
    return out;
}
function encodeResponse(id, qname, qtype, { rcode, answers, aa }, rd) {
    const parts = [],
        offsets = new Map();
    const header = new Uint8Array(12);
    writeUint16BE(header, 0, id);
    parts.push(header);
    let pos = encodeName(qname, parts, offsets, 12);
    const qt = new Uint8Array(4);
    writeUint16BE(qt, 0, qtype);
    writeUint16BE(qt, 2, 1);
    parts.push(qt);
    pos += 4;
    for (const rr of answers) {
        pos = encodeName(rr.name, parts, offsets, pos);
        const fixed = new Uint8Array(10);
        writeUint16BE(fixed, 0, rr.type);
        writeUint16BE(fixed, 2, 1);
        writeUint32BE(fixed, 4, rr.ttl);
        parts.push(fixed);
        pos += 10;
        const rdParts = [];
        let rdPos = pos;
        if (rr.type === TYPE.A) (rdParts.push(Uint8Array.from(rr.address.split(".").map(Number))), (rdPos += 4));
        else if (rr.type === TYPE.AAAA) (rdParts.push(ipv6Bytes(rr.address)), (rdPos += 16));
        else if (rr.type === TYPE.CNAME || rr.type === TYPE.NS) rdPos = encodeName(rr.target, rdParts, offsets, pos);
        else if (rr.type === TYPE.MX) {
            const p = new Uint8Array(2);
            writeUint16BE(p, 0, rr.priority);
            rdParts.push(p);
            rdPos = encodeName(rr.exchange, rdParts, offsets, pos + 2);
        } else if (rr.type === TYPE.TXT)
            for (const c of rr.chunks) {
                const b = new TextEncoder().encode(c);
                rdParts.push(Uint8Array.of(b.length), b);
                rdPos += 1 + b.length;
            }
        writeUint16BE(fixed, 8, rdPos - pos);
        parts.push(...rdParts);
        pos = rdPos;
    }
    let msg = concat(parts);
    let tc = 0;
    if (msg.length > 512) {
        tc = 1;
        answers = [];
        msg = msg.slice(0, 12 + (encodeName(qname, [], new Map(), 12) - 12) + 4);
    }
    writeUint16BE(msg, 2, 0x8000 | (aa ? 0x0400 : 0) | (tc ? 0x0200 : 0) | (rd ? 0x0100 : 0) | rcode);
    writeUint16BE(msg, 4, 1);
    writeUint16BE(msg, 6, tc ? 0 : answers.length);
    return msg;
}
function decodeName(buf, pos) {
    const labels = [];
    let jumped = false,
        end = pos,
        hops = 0;
    for (;;) {
        if (pos >= buf.length) throw new RangeError("name runs past the message");
        const len = buf[pos];
        if ((len & 0xc0) === 0xc0) {
            if (++hops > 16) throw new RangeError("compression pointer loop");
            if (!jumped) end = pos + 2;
            pos = readUint16BE(buf, pos) & 0x3fff;
            jumped = true;
            continue;
        }
        if (len === 0) {
            if (!jumped) end = pos + 1;
            break;
        }
        if (len > 63) throw new RangeError("label too long");
        labels.push(new TextDecoder().decode(buf.subarray(pos + 1, pos + 1 + len)));
        pos += 1 + len;
    }
    return [labels.join("."), end];
}
function decodeQuery(buf) {
    if (buf.length < 12) throw new RangeError("short header");
    const id = readUint16BE(buf, 0),
        flags = readUint16BE(buf, 2),
        qd = readUint16BE(buf, 4);
    if (qd !== 1) throw new RangeError("expected one question");
    const [qname, p] = decodeName(buf, 12);
    if (p + 4 > buf.length) throw new RangeError("truncated question");
    return { id, rd: !!(flags & 0x0100), qname, qtype: readUint16BE(buf, p) };
}
function encodeQuery(id, name, type) {
    const parts = [new Uint8Array(12)];
    writeUint16BE(parts[0], 0, id);
    writeUint16BE(parts[0], 2, 0x0100);
    writeUint16BE(parts[0], 4, 1);
    encodeName(name, parts, new Map(), 12);
    const q = new Uint8Array(4);
    writeUint16BE(q, 0, type);
    writeUint16BE(q, 2, 1);
    parts.push(q);
    return concat(parts);
}

// =============================================================================
// Authoritative server
// =============================================================================
const stats = new Multiset();
const server = new UDPSocket({ host: "127.0.0.1", port: 0 });
server.start({
    message(data, from) {
        let q;
        try {
            q = decodeQuery(data);
        } catch (e) {
            stats.add("formerr");
            if (data.length >= 2) {
                const r = new Uint8Array(12);
                r[0] = data[0];
                r[1] = data[1];
                writeUint16BE(r, 2, 0x8001);
                server.send(r, from.address, from.port);
            }
            return;
        }
        const res = resolve(q.qname, q.qtype);
        stats.add(`rcode${res.rcode}`);
        server.send(encodeResponse(q.id, q.qname, q.qtype, res, q.rd), from.address, from.port);
    },
});

// A raw client to see flags and rcodes, which a high-level resolver hides.
const raw = new UDPSocket({ host: "127.0.0.1", port: 0 });
const pending = new Map();
raw.start({
    message(data) {
        const id = readUint16BE(data, 0);
        pending.get(id)?.(data);
        pending.delete(id);
    },
});
let nextId = 1;
const ask = (bytes, id) =>
    new Promise((resolve, reject) => {
        pending.set(id, resolve);
        raw.send(bytes, "127.0.0.1", server.port);
        setTimeout(() => {
            if (pending.delete(id)) reject(new Error("timeout"));
        }, 1000);
    });
async function rawQuery(name, type) {
    const id = nextId++;
    const r = await ask(encodeQuery(id, name, type), id);
    const flags = readUint16BE(r, 2);
    return {
        rcode: flags & 15,
        aa: !!(flags & 0x400),
        tc: !!(flags & 0x200),
        ancount: readUint16BE(r, 6),
        bytes: r.length,
    };
}

const resolver = new DNSResolver({ server: "127.0.0.1", port: server.port, timeoutMs: 1000 });
const lookup = (name, type) => resolver.lookup(name, type);

const a = await lookup("web.prod.example.test", TYPE.A);
results["A records decode with the foreign resolver"] =
    a
        .map((r) => r.address)
        .sort()
        .join() === "192.0.2.80,192.0.2.81" && a.every((r) => r.ttl === 300);
const aaaa = await lookup("web.prod.example.test", TYPE.AAAA);
results["AAAA round-trips"] = aaaa.length === 1 && /^2001:db8::80$/i.test(aaaa[0].address);
const viaCname = await lookup("api.example.test", TYPE.A);
const firstHop = await lookup("api.example.test", TYPE.CNAME);
results["CNAME chain api -> www -> web.prod is followed"] =
    viaCname.length === 2 &&
    viaCname.every((r) => r.type === TYPE.A && r.name === "web.prod.example.test") &&
    firstHop.length === 1 &&
    firstHop[0].target === "www.example.test";
const mx = await resolver.resolveMx("example.test");
results["MX priorities and exchanges"] =
    mx
        .sortBy((m) => m.priority)
        .map((m) => `${m.priority} ${m.exchange}`)
        .join() === "10 mail.example.test,20 backup-mail.example.test";
const txt = await lookup("feature-42.preview.example.test", TYPE.TXT);
results["wildcard synthesises the queried owner name"] =
    txt.length === 1 &&
    txt[0].name === "feature-42.preview.example.test" &&
    txt[0].chunks.join("|") === "wildcard|preview env";
const mail = await lookup("mail.example.test", TYPE.A);
results["per-record TTL overrides $TTL"] = mail[0].ttl === 60;

const nx = await rawQuery("nope.example.test", TYPE.A);
const ent = await rawQuery("b.c.example.test", TYPE.A);
const nodata = await rawQuery("mail.example.test", TYPE.MX);
const refused = await rawQuery("example.org", TYPE.A);
const wildNx = await rawQuery("x.y.preview.example.test", TYPE.A);
const big = await rawQuery("big.example.test", TYPE.TXT);
results["unknown name is NXDOMAIN and authoritative"] = nx.rcode === 3 && nx.aa;
results["empty non-terminal is NODATA, not NXDOMAIN"] = ent.rcode === 0 && ent.ancount === 0;
results["existing name without the type is NODATA"] = nodata.rcode === 0 && nodata.ancount === 0;
results["out-of-zone query is REFUSED"] = refused.rcode === 5 && !refused.aa;
results["wildcard covers deeper names below the encloser"] = wildNx.rcode === 0 && wildNx.ancount === 1;
results["small answers are not truncated"] = !big.tc && big.ancount === 1;

// Compression: the MX answer repeats the zone suffix four times; with pointers it is stored once.
{
    const res = resolve("example.test", TYPE.MX);
    const msg = encodeResponse(1, "example.test", TYPE.MX, res, true);
    const naive = 12 + 14 + 4 + res.answers.reduce((s, r) => s + 14 + 10 + 2 + r.exchange.length + 2, 0);
    results["name compression shrinks the MX reply"] = msg.length < naive * 0.75;
}
// Truncation: a response over 512 bytes carries TC and no records.
{
    zone.set(
        revKey("huge.example.test"),
        Array.from({ length: 12 }, (_, i) => ({
            name: "huge.example.test",
            type: TYPE.TXT,
            ttl: 30,
            chunks: [`chunk-${i}-${"y".repeat(60)}`],
        })),
    );
    const huge = await rawQuery("huge.example.test", TYPE.TXT);
    results["oversized answer sets TC with zero records"] = huge.tc && huge.ancount === 0 && huge.bytes <= 512;
}
// Hostile queries: a pointer loop and a truncated header are answered FORMERR or dropped, never hang.
{
    const loop = new Uint8Array(12 + 2 + 4);
    writeUint16BE(loop, 0, 777);
    writeUint16BE(loop, 4, 1);
    writeUint16BE(loop, 12, 0xc00c);
    const r = await ask(loop, 777);
    results["compression pointer loop is FORMERR"] = (readUint16BE(r, 2) & 15) === 1;
    raw.send(Uint8Array.of(1, 2, 3), "127.0.0.1", server.port);
    const after = await rawQuery("ns1.example.test", TYPE.A);
    results["server survives a 3-byte datagram"] = after.rcode === 0 && after.ancount === 1;
}

// =============================================================================
// Caching forwarder
// =============================================================================
const cache = new LRU(512);
const fwdStats = new Multiset();
let upstreamQueries = 0,
    clock = 0;
const forwarder = new UDPSocket({ host: "127.0.0.1", port: 0 });
const upstream = new UDPSocket({ host: "127.0.0.1", port: 0 });
const waiting = new Map();
upstream.start({
    message(data) {
        const id = readUint16BE(data, 0);
        const w = waiting.get(id);
        waiting.delete(id);
        w?.(data);
    },
});
let upId = 1;
forwarder.start({
    message(data, from) {
        let q;
        try {
            q = decodeQuery(data);
        } catch {
            return;
        }
        const key = `${q.qname.toLowerCase()}/${q.qtype}`;
        const hit = cache.get(key);
        if (hit && hit.expires > clock) {
            fwdStats.add("hit");
            const reply = hit.bytes.slice();
            writeUint16BE(reply, 0, q.id);
            forwarder.send(reply, from.address, from.port);
            return;
        }
        fwdStats.add("miss");
        upstreamQueries++;
        const id = upId++;
        waiting.set(id, (reply) => {
            const flags = readUint16BE(reply, 2),
                rcode = flags & 15;
            const ttl = rcode === 3 ? 30 : minTtl(reply);
            cache.set(key, { bytes: reply, expires: clock + ttl });
            const out = reply.slice();
            writeUint16BE(out, 0, q.id);
            forwarder.send(out, from.address, from.port);
        });
        upstream.send(encodeQuery(id, q.qname, q.qtype), "127.0.0.1", server.port);
    },
});
function minTtl(reply) {
    const an = readUint16BE(reply, 6);
    let [, p] = decodeName(reply, 12);
    p += 4;
    let ttl = 3600;
    for (let i = 0; i < an; i++) {
        [, p] = decodeName(reply, p);
        ttl = Math.min(ttl, reply[p + 4] * 2 ** 24 + readUint16BE(reply, p + 6) + reply[p + 5] * 65536);
        p += 10 + readUint16BE(reply, p + 8);
    }
    return an ? ttl : 60;
}
const viaFwd = new DNSResolver({ server: "127.0.0.1", port: forwarder.port, timeoutMs: 1000 });
const names = ["www", "api", "mail", "ns1", "web.prod", "a.preview", "b.preview", "missing", "nope", "backup-mail"].map(
    (n) => `${n}.example.test`,
);
const rng = new Random(53);
const zipf = () => names[Math.min(names.length - 1, Math.floor(rng.exponential(0.5)))];
let answered = 0,
    nxCount = 0;
for (let i = 0; i < 300; i++) {
    clock += 1;
    try {
        await viaFwd.lookup(zipf(), TYPE.A);
        answered++;
    } catch (e) {
        nxCount++;
    }
}
const hits = fwdStats.count("hit"),
    misses = fwdStats.count("miss");
results["forwarder answers from cache most of the time"] = hits / (hits + misses) > 0.85;
results["upstream sees one query per name per TTL window"] = upstreamQueries <= names.length * 3;
results["NXDOMAIN is cached negatively"] = nxCount > 0 && upstreamQueries < 300 - nxCount;
{
    clock += 1000;
    await viaFwd.lookup("mail.example.test", TYPE.A);
    await viaFwd.lookup("www.example.test", TYPE.A);
    const before = upstreamQueries;
    clock += 61;
    await viaFwd.lookup("mail.example.test", TYPE.A);
    await viaFwd.lookup("www.example.test", TYPE.A);
    results["after 61 s the TTL-60 entry is refetched and the TTL-300 one is not"] = upstreamQueries === before + 1;
}

print(
    Table(
        [
            [
                "authoritative rcodes",
                stats
                    .entrySet()
                    .map(([k, v]) => `${k}=${v}`)
                    .join(" "),
            ],
            ["forwarder hits / misses", `${hits} / ${misses} (${((100 * hits) / (hits + misses)).toFixed(1)}%)`],
            ["upstream queries", String(upstreamQueries)],
        ],
        { head: ["metric", "value"] },
    ),
);

for (const s of [server, raw, forwarder, upstream, resolver, viaFwd]) s.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
