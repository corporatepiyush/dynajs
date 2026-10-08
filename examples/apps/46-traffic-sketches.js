// 46 · Streaming traffic statistics — unique visitors, heavy hitters and "seen before?" in fixed memory.
//
// WHAT IT SHOWS
//   - dyna:structures HyperLogLog: count distinct values in kilobytes, whatever the cardinality
//   - CountMinSketch + a small candidate heap: the top talkers of a stream without a map of everyone
//   - BloomFilter: first-visit detection with no per-visitor state
//   - merging sketches from several servers, and persisting them between runs
//   - the trade: answers are approximate, and the error is known in advance
//
// RUN      dynajs examples/apps/46-traffic-sketches.js

import { HyperLogLog, CountMinSketch, BloomFilter } from "dyna:structures";
import { Random } from "dyna:random";

class TrafficStats {
    constructor(topN = 5) {
        this.unique = new HyperLogLog(14);            // 16 KiB, about 1% error
        this.freq = new CountMinSketch(4096, 5);      // never undercounts; overcounts slightly
        this.seen = new BloomFilter(1 << 21, 5);      // 256 KiB of bits
        this.topN = topN;
        this.top = new Map();                         // candidate heavy hitters: ip -> estimate
        this.requests = 0; this.firstVisits = 0;
    }

    record(ip) {
        this.requests++;
        this.unique.add(ip);
        if (!this.seen.mayContain(ip)) { this.firstVisits++; this.seen.add(ip); }
        this.freq.add(ip);

        // Keep only a handful of candidates. An address enters when its
        // estimated count beats the smallest candidate's.
        const estimate = this.freq.count(ip);
        if (this.top.has(ip) || this.top.size < this.topN * 4) this.top.set(ip, estimate);
        else {
            let minKey = null, min = Infinity;
            for (const [k, v] of this.top) if (v < min) { min = v; minKey = k; }
            if (estimate > min) { this.top.delete(minKey); this.top.set(ip, estimate); }
        }
    }

    report() {
        return {
            requests: this.requests,
            uniqueVisitors: Math.round(this.unique.count()),
            firstVisits: this.firstVisits,
            topTalkers: [...this.top].sort((a, b) => b[1] - a[1]).slice(0, this.topN).map(([ip, n]) => ({ ip, requests: n })),
        };
    }

    // Combine another node's statistics into this one. Sketches of the same
    // shape merge exactly: the result equals one sketch that saw both streams.
    merge(other) {
        this.unique.merge(other.unique);
        this.freq.merge(other.freq);
        this.requests += other.requests;
        for (const ip of other.top.keys()) this.top.set(ip, this.freq.count(ip));
        for (const ip of this.top.keys()) this.top.set(ip, this.freq.count(ip));
        return this;
    }
}

// ---- simulated traffic -----------------------------------------------------
// 200,000 requests from 50,000 ordinary visitors, plus three crawlers that
// each account for a large share. Exact answers are tracked for comparison.
const rng = new Random(5);
const ip = (n) => `10.${(n >> 16) & 255}.${(n >> 8) & 255}.${n & 255}`;
const crawlers = { "198.51.100.7": 9000, "203.0.113.42": 6000, "192.0.2.99": 3500 };
const stream = [];
for (const [addr, n] of Object.entries(crawlers)) for (let i = 0; i < n; i++) stream.push(addr);
while (stream.length < 200000) stream.push(ip(rng.nextBounded(50000)));
rng.shuffle(stream);

const exact = new Map();
const nodeA = new TrafficStats(), nodeB = new TrafficStats();
stream.forEach((addr, i) => {
    exact.set(addr, (exact.get(addr) ?? 0) + 1);
    (i % 2 ? nodeA : nodeB).record(addr);            // two servers each see half
});

const combined = new TrafficStats().merge(nodeA).merge(nodeB);
const report = combined.report();
console.log(`requests ${report.requests}, unique visitors ~${report.uniqueVisitors} (exact ${exact.size})`);
for (const t of report.topTalkers.slice(0, 3)) console.log(`  ${t.ip.padEnd(15)} ~${t.requests} (exact ${exact.get(t.ip)})`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const error = Math.abs(report.uniqueVisitors - exact.size) / exact.size;
check(error < 0.03, `distinct count within 3% (${(error * 100).toFixed(2)}%)`);
check(report.topTalkers.slice(0, 3).map((t) => t.ip).join() === Object.keys(crawlers).join(), "the three crawlers lead the ranking");
for (const [addr, n] of Object.entries(crawlers)) {
    const est = combined.freq.count(addr);
    check(est >= n && est <= n * 1.05, `count-min never undercounts and stays close for ${addr} (${est} vs ${n})`);
}
// First-visit detection on one node: Bloom false positives only ever LOWER the count.
const single = new TrafficStats();
for (const addr of stream) single.record(addr);
check(single.firstVisits <= exact.size && single.firstVisits > exact.size * 0.98, `first visits ${single.firstVisits} of ${exact.size}`);
check(Math.round(single.unique.count()) === report.uniqueVisitors, "merged sketches equal a single sketch over the whole stream");

// The memory story: the exact map holds 50,000 keys; the sketches do not grow.
const bytes = combined.unique.serialize().length + combined.freq.serialize().length;
check(bytes < 200 * 1024, `unique + frequency state is ${Math.round(bytes / 1024)} KiB regardless of traffic`);

// Persist and restore: the restored sketch keeps counting where it left off.
const restored = HyperLogLog.deserialize(combined.unique.serialize());
restored.add("a brand new visitor");
check(Math.round(restored.count()) >= report.uniqueVisitors, "a restored sketch continues from its saved state");
console.log(`self-test passed: ${Math.round(bytes / 1024)} KiB of sketch state for ${exact.size} distinct visitors`);
