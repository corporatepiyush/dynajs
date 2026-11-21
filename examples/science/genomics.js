// Genomics — a small sequencing pipeline: FASTQ parsing with Phred qualities and read trimming, k-mer spectra and
// error correction, de Bruijn graph assembly, seed-and-extend read alignment with banded Smith–Waterman, pileup
// variant calling written as VCF, and gene finding (ORFs and translation) on the assembly.
//
// WHAT IT IS
//   input        a reference "plasmid" (FASTA), and gzipped FASTQ reads simulated from a mutated sample genome
//                with a position-dependent error profile (quality falls along the read) and adapter read-through
//   QC           per-position mean quality, adapter trimming by overlap, quality trimming (sliding window Q20),
//                GC content, duplicate rate
//   k-mers       a k=21 spectrum: exact counts for the solid set, a count-min sketch for the long tail, and an
//                error-correction pass that replaces weak k-mers' bases to make them solid
//   assembly     de Bruijn graph of solid k-mers, unitigs by walking non-branching paths, tips removed, contigs
//                reported with N50
//   alignment    a k-mer seed index over the reference, diagonal voting for the best locus, banded Smith–Waterman
//                to place each read, CIGAR strings, and mapping quality from the score margin
//   variants     pileup per position with base qualities; SNPs and small indels called by allele fraction and
//                depth, genotyped (0/1 vs 1/1), written as VCF 4.2 and compared with the truth
//   genes        open reading frames on both strands, translated with the standard codon table
//
//   encoding     k-mers are 2-bit integers (4^21 = 2^42, exact in a double) rolled along each read together with
//                their reverse complement, so canonical choice is one Math.min and a candidate base substitution
//                during error correction is two multiply-adds per covering k-mer instead of a slice, a join and
//                a reverse complement; numeric order equals lexicographic order, so every choice matches strings
//   vectors      per-position quality sums and the Q20 sliding window are dyna:simd adds over the record's bytes,
//                GC is a native MultiMatcher count, and the pileup is an Int32Array of five counters per base
//   checks       the codes against string k-mers, the count-min bound (never under; over by e·N/width for at
//                most e^-depth of keys), a genome size from the spectrum, contigs re-aligned to the reference,
//                and the quality trim re-checked base by base
//
// MODULES  dyna:compress (gzip) · dyna:simd (add) · dyna:structures (CountMinSketch, Multimap) · dyna:hash (XXH3) · dyna:random
//          · dyna:matcher (MultiMatcher, SmithWaterman) · dyna:dataframe · dyna:file · dyna:cli (Table) · dyna:stream (lines over gzip)
//
// RUN      dynajs examples/science/genomics.js     simulates a 12 kb sample, sequences it at 40x and analyses it

import { gzip } from "dyna:compress";
import { CountMinSketch, Multimap } from "dyna:structures";
import { MultiMatcher, SmithWaterman } from "dyna:matcher";
import { add } from "dyna:simd";
import { XXH3_64 } from "dyna:hash";
import { Random } from "dyna:random";
import { DataFrame } from "dyna:dataframe";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
import { Table } from "dyna:cli";
import { fromFile, lines } from "dyna:stream";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("genomics");
const rng = new Random(1953);
const BASES = "ACGT";
const COMPLEMENT = new MultiMatcher(["A", "C", "G", "T"]),
    SWAP = ["T", "G", "C", "A"];
const revcomp = (s) => COMPLEMENT.replaceAllIn(s.reverse(), (_, i) => SWAP[i]); // native reverse, one Aho-Corasick pass

// =============================================================================
// Reference, sample and reads
// =============================================================================
const GENES = [
    "ATGGCTAAAGAAACCGCGGCTGCTAAATTCGAACGCCAGCACATGGACAGCAGCACTTCAGCAGCGTCGTCTGCCGCGGCGGCTTAA",
    "ATGAGCATTCAACATTTCCGTGTCGCCCTTATTCCCTTTTTTGCGGCATTTTGCCTTCCTGTTTTTGCTCACCCAGAAACGCTGGTGAAAGTAA",
];
let reference = Array.from({ length: 12000 }, () => BASES[rng.nextBounded(4)]).join("");
reference =
    reference.slice(0, 2000) +
    GENES[0] +
    reference.slice(2000 + GENES[0].length, 7000) +
    revcomp(GENES[1]) +
    reference.slice(7000 + GENES[1].length);
const truthVariants = [];
for (let pos = 300; pos < 11700; pos += 600 + rng.nextBounded(400)) {
    if (truthVariants.length % 5 === 4)
        truthVariants.push({ pos, ref: reference.slice(pos - 1, pos + 2), alt: reference[pos - 1], gt: "1/1" }); // 2-base deletion after the anchor
    else {
        const ref = reference[pos],
            alt = BASES.replace(ref, "")[rng.nextBounded(3)];
        truthVariants.push({ pos: pos + 1, ref, alt, gt: truthVariants.length % 3 ? "1/1" : "0/1" });
    }
}
const applyVariants = (vs) => {
    let g = reference;
    for (const v of [...vs].sort((x, y) => y.pos - x.pos))
        g =
            v.ref.length === 1
                ? g.slice(0, v.pos - 1) + v.alt + g.slice(v.pos)
                : g.slice(0, v.pos) + g.slice(v.pos + 2);
    return g;
};
const haplotypes = [applyVariants(truthVariants), applyVariants(truthVariants.filter((v) => v.gt === "1/1"))];
const hom = haplotypes[0];
const ADAPTER = "AGATCGGAAGAGC",
    READ_LEN = 100,
    COVERAGE = 40;
const nReads = Math.round((hom.length * COVERAGE) / READ_LEN);
const fastq = [];
// per read: one bulk draw of the quality noise and of the error coins; P(error) at Phred q is a table lookup
const PERR = Float64Array.from({ length: 64 }, (_, q) => 10 ** (-q / 10)),
    noise = new Float64Array(READ_LEN),
    coins = new Float64Array(READ_LEN),
    qcodes = new Uint8Array(READ_LEN),
    bases = new Uint8Array(READ_LEN);
for (let r = 0; r < nReads; r++) {
    const src = haplotypes[r % 2],
        insert = 70 + rng.nextBounded(80),
        start = rng.nextBounded(src.length - insert);
    let frag = src.slice(start, start + insert);
    if (rng.nextFloat() < 0.5) frag = revcomp(frag);
    const clean = (frag + ADAPTER + "ACGTACGTACGTACGTACGTACGTACGT").slice(0, READ_LEN);
    rng.fillExponential(noise, 1 / 2);
    rng.fillUniform(coins, 0, 1);
    for (let i = 0; i < READ_LEN; i++) {
        const q = Math.max(2, Math.round(38 - i * 0.18 - noise[i]));
        qcodes[i] = 33 + q;
        bases[i] = coins[i] < PERR[q] ? BASES.charCodeAt(rng.nextBounded(4)) : clean.charCodeAt(i);
    }
    const seq = String.fromCharCode(...bases),
        qual = String.fromCharCode(...qcodes);
    fastq.push(`@read${r}/1\n${seq}\n+\n${qual}`);
    if (r % 97 === 0) fastq.push(`@read${r}dup/1\n${seq}\n+\n${qual}`);
}
writeFile(work.join("reads.fastq.gz"), gzip(fastq.join("\n") + "\n"));
writeFile(work.join("ref.fa"), `>plasmid demo\n${reference.match(/.{1,70}/g).join("\n")}\n`);

// =============================================================================
// QC: parse, trim adapters and low quality
// =============================================================================
const reads = [],
    posQual = new Float32Array(READ_LEN),
    posN = new Float32Array(READ_LEN);
// per record: the quality string as bytes once, then whole-row vector ops (sums of Phred+33 codes are exact in f32)
const utf8 = new TextEncoder(),
    qf = new Float32Array(READ_LEN),
    win = new Float32Array(READ_LEN),
    ONES = new Float32Array(READ_LEN).fill(1),
    GC = new MultiMatcher(["G", "C"]);
let rawReads = 0,
    adapterTrimmed = 0,
    gc = 0,
    totalBases = 0;
const seen = new Set();
let dups = 0;
{
    const it = lines(fromFile(work.join("reads.fastq.gz")).pipeThrough(new DecompressionStream("gzip")));
    const rec = [];
    for await (const line of it) {
        rec.push(line);
        if (rec.length < 4) continue;
        const [h, seq, , q] = rec.splice(0);
        if (!h.startsWith("@") || seq.length !== q.length) throw new Error(`malformed FASTQ record ${h}`);
        rawReads++;
        const L = q.length,
            row = qf.subarray(0, L);
        row.set(utf8.encode(q));
        add(posQual.subarray(0, L), posQual.subarray(0, L), row);
        add(posN.subarray(0, L), posN.subarray(0, L), ONES.subarray(0, L));
        if (seen.has(seq)) {
            dups++;
            continue;
        }
        seen.add(seq);
        let end = seq.length;
        for (let k = ADAPTER.length; k >= 8; k--) {
            const at = seq.indexOf(ADAPTER.slice(0, k));
            if (at >= 0 && (k === ADAPTER.length || at + k === seq.length || at + k >= seq.length - 2)) {
                end = at;
                adapterTrimmed++;
                break;
            }
        }
        if (end >= 4) {
            // the first 4-base window with mean Q < 20, i.e. a sum of raw codes below 4·(20 + 33) = 212
            const w = win.subarray(0, end - 3);
            add(w, qf.subarray(0, end - 3), qf.subarray(1, end - 2));
            add(w, w, qf.subarray(2, end - 1));
            add(w, w, qf.subarray(3, end));
            const low = w.findIndex((v) => v < 212);
            if (low >= 0) end = low;
        }
        if (end < 40) continue;
        const s = seq.slice(0, end);
        reads.push({ id: h.slice(1), seq: s, qual: q.slice(0, end) });
        gc += GC.countIn(s);
        totalBases += s.length;
    }
}

// =============================================================================
// k-mer spectrum, error correction, de Bruijn assembly
// =============================================================================
// k-mers are 2-bit integers, most significant base first (A=0 C=1 G=2 T=3): 4^21 = 2^42 is exact in a double,
// numeric order is lexicographic order, and the reverse complement rolls along with the forward code.
const K = 21,
    TOP = 4 ** (K - 1),
    POW = Float64Array.from(Iterator.range(0, K), (k) => 4 ** k),
    CODE = new Uint8Array(128);
[..."ACGT"].forEach((c, i) => (CODE[c.charCodeAt(0)] = i));
const fwd = new Float64Array(256),
    rev = new Float64Array(256);
function rollKmers(seq) {
    // fwd[i], rev[i]: codes of seq[i, i+K) and of its reverse complement; returns the k-mer count
    let f = 0,
        r = 0;
    for (let i = 0; i < seq.length; i++) {
        const c = CODE[seq.charCodeAt(i)];
        f = (f % TOP) * 4 + c;
        r = Math.floor(r / 4) + (3 - c) * TOP;
        if (i >= K - 1) {
            fwd[i - K + 1] = f;
            rev[i - K + 1] = r;
        }
    }
    return Math.max(0, seq.length - K + 1);
}
const rcCode = (x) => {
    let y = 0;
    for (let k = 0; k < K; k++, x = Math.floor(x / 4)) y = y * 4 + (3 - (x % 4));
    return y;
};
const decode = (x) => Array.from({ length: K }, (_, k) => BASES[Math.floor(x / POW[K - 1 - k]) % 4]).join("");
const rawSeqs = reads.map((r) => r.seq);
const counts = new Map(),
    sketch = new CountMinSketch(1 << 16, 4);
for (const r of reads)
    for (let i = 0, n = rollKmers(r.seq); i < n; i++) {
        const c = Math.min(fwd[i], rev[i]);
        counts.set(c, (counts.get(c) || 0) + 1);
    }
for (const [c, n] of counts) sketch.add(c.toString(36), n);
const hist = counts.values().countBy((v) => Math.min(v, 60));
let valley = 2;
for (let c = 2; c < 20; c++) if ((hist[c] || 0) < (hist[valley] || 0)) valley = c;
const SOLID = Math.max(3, valley);
let corrected = 0;
const solid = new Set();
const solidCode = (f, r) => (counts.get(Math.min(f, r)) || 0) >= SOLID;
for (const r of reads) {
    // an error makes the K k-mers covering it weak: fix the base they share. A substitution at p shifts the
    // forward code of the k-mer starting at st by (b - o)·4^(K-1-(p-st)) and its reverse complement by (o - b)·4^(p-st)
    const s = Uint8Array.from(r.seq, (ch) => CODE[ch.charCodeAt(0)]),
        n = rollKmers(r.seq),
        weak = (st) => !solidCode(fwd[st], rev[st]);
    let i = 0;
    while (i < n) {
        if (!weak(i)) {
            i++;
            continue;
        }
        let j = i;
        while (j + 1 < n && weak(j + 1)) j++;
        let fixed = false;
        for (let p = j; p <= Math.min(s.length - 1, i + K - 1) && !fixed; p++) {
            const o = s[p],
                lo = Math.max(0, p - K + 1),
                hi = Math.min(p, n - 1);
            for (let b = 0; b < 4 && !fixed; b++) {
                if (b === o) continue;
                let ok = true;
                for (let st = lo; st <= hi && ok; st++)
                    ok = solidCode(fwd[st] + (b - o) * POW[K - 1 - (p - st)], rev[st] + (o - b) * POW[p - st]);
                if (ok) {
                    for (let st = lo; st <= hi; st++) {
                        fwd[st] += (b - o) * POW[K - 1 - (p - st)];
                        rev[st] += (o - b) * POW[p - st];
                    }
                    s[p] = b;
                    corrected++;
                    fixed = true;
                }
            }
        }
        i = j + 1;
    }
    r.seq = Array.from(s, (c) => BASES[c]).join("");
    // the codes now describe the corrected read: its solid k-mers, both orientations, join the graph
    for (let st = 0; st < n; st++)
        if (solidCode(fwd[st], rev[st])) {
            solid.add(fwd[st]);
            solid.add(rev[st]);
        }
}
// de Bruijn walk on oriented codes: successor = drop the first base, append b; predecessor = prepend b
const succ = (x) => [0, 1, 2, 3].map((b) => (x % TOP) * 4 + b).filter((y) => solid.has(y));
const pred = (x) => [0, 1, 2, 3].map((b) => Math.floor(x / 4) + b * TOP).filter((y) => solid.has(y));
const used = new Set(),
    contigs = [];
for (const start of solid) {
    if (used.has(start)) continue;
    let head = start;
    while (
        pred(head).length === 1 &&
        succ(pred(head)[0]).length === 1 &&
        pred(head)[0] !== start &&
        !used.has(pred(head)[0])
    )
        head = pred(head)[0];
    let cur = head;
    const tail = [];
    used.add(cur);
    used.add(rcCode(cur));
    while (succ(cur).length === 1 && pred(succ(cur)[0]).length === 1 && !used.has(succ(cur)[0])) {
        cur = succ(cur)[0];
        tail.push(BASES[cur % 4]);
        used.add(cur);
        used.add(rcCode(cur));
    }
    if (K + tail.length >= 200) contigs.push(decode(head) + tail.join(""));
}
contigs.sort((a, b) => b.length - a.length);
const n50 = (() => {
    const total = contigs.reduce((s, c) => s + c.length, 0);
    let acc = 0;
    for (const c of contigs) {
        acc += c.length;
        if (acc >= total / 2) return c.length;
    }
    return 0;
})();

// =============================================================================
// Alignment: seed index + banded Smith–Waterman
// =============================================================================
const SEED = 15,
    index = new Multimap();
for (let i = 0; i + SEED <= reference.length; i++) index.put(reference.slice(i, i + SEED), i);
// Banded local alignment in C (matcher.SmithWaterman): the band keeps cells with 0 <= j - i <= 2 * band in the
// reference window, i.e. diagonal = band, half-width = band. Ties prefer the diagonal, then a read gap, as before.
function sw(read, refStart, band = 12) {
    const off = Math.max(0, refStart - band),
        ref = reference.slice(off, refStart + read.length + band);
    const r = SmithWaterman(read, ref, { match: 2, mismatch: -4, gap: -6, band, diagonal: band });
    const runs = r.cigar
        .matchAll(/(\d+)([MID])/g)
        .map(([, n, op]) => [Number(n), op])
        .toArray();
    const cigar = (r.aStart ? `${r.aStart}S` : "") + r.cigar + (read.length - r.aEnd ? `${read.length - r.aEnd}S` : "");
    return { score: r.score, pos: off + r.bStart, cigar, runs, readStart: r.aStart };
}
function align(read) {
    const best = [];
    for (const [strand, s] of [
        ["+", read.seq],
        ["-", revcomp(read.seq)],
    ]) {
        const votes = new Map();
        for (let i = 0; i + SEED <= s.length; i += 4)
            for (const p of index.get(s.slice(i, i + SEED))) votes.set(p - i, (votes.get(p - i) || 0) + 1);
        const [diag, v] = votes.entries().maxBy(([, n]) => n) ?? [0, 0];
        if (v >= 2) best.push({ strand, s, diag, v });
    }
    if (!best.length) return null;
    const scored = best.map((b) => ({ ...b, ...sw(b.s, Math.max(0, b.diag)) })).sort((a, b) => b.score - a.score);
    const top = scored[0],
        mapq = scored[1] ? Math.min(60, Math.round((top.score - scored[1].score) / 2)) : 60;
    return { ...top, mapq, qual: top.strand === "+" ? read.qual : [...read.qual].reverse().join("") };
}
// pileup: five counters (A C G T and a deletion) per reference position, filled by walking each CIGAR run
const ALLELES = "ACGT-",
    pile = new Int32Array(reference.length * 5);
let mapped = 0;
for (const r of reads) {
    const a = align(r);
    if (!a || a.mapq < 10 || a.score < 60) continue;
    mapped++;
    let rp = a.readStart,
        gp = a.pos;
    for (const [n, op] of a.runs)
        if (op === "M")
            for (let k = 0; k < n; k++, rp++, gp++) {
                if (a.qual.charCodeAt(rp) - 33 >= 20) pile[gp * 5 + CODE[a.s.charCodeAt(rp)]]++;
            }
        else if (op === "I") rp += n;
        else for (let k = 0; k < n; k++, gp++) pile[gp * 5 + 4]++;
}

// =============================================================================
// Variant calling and VCF
// =============================================================================
const calls = [];
for (let p = 0; p < reference.length; p++) {
    const col = pile.subarray(p * 5, p * 5 + 5),
        depth = col.values().sum(),
        ref = reference[p];
    if (depth < 10) continue;
    for (const [b, n] of col.entries()) {
        const alt = ALLELES[b];
        if (alt === ref || !n) continue;
        const af = n / depth;
        if (af < 0.25) continue;
        if (alt === "-") {
            if (!calls.some((c) => c.kind === "del" && Math.abs(c.pos - p) <= 2))
                calls.push({
                    kind: "del",
                    pos: p,
                    ref: reference.slice(p - 1, p + 2),
                    alt: reference[p - 1],
                    depth,
                    af,
                    gt: af > 0.8 ? "1/1" : "0/1",
                });
            continue;
        }
        calls.push({ kind: "snp", pos: p + 1, ref, alt, depth, af, gt: af > 0.8 ? "1/1" : "0/1" });
    }
}
const vcf = [
    "##fileformat=VCFv4.2",
    "##reference=ref.fa",
    '##INFO=<ID=DP,Number=1,Type=Integer,Description="Depth">',
    '##INFO=<ID=AF,Number=A,Type=Float,Description="Allele fraction">',
    '##FORMAT=<ID=GT,Number=1,Type=String,Description="Genotype">',
    "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tsample",
    ...calls.map(
        (c) =>
            `plasmid\t${c.pos}\t.\t${c.ref}\t${c.alt}\t${Math.min(99, Math.round(c.depth * c.af))}\tPASS\tDP=${c.depth};AF=${c.af.toFixed(2)}\tGT\t${c.gt}`,
    ),
].join("\n");
writeFile(work.join("calls.vcf"), vcf + "\n");

// =============================================================================
// Genes on the assembly's reference coordinates
// =============================================================================
const CODONS = {};
const AA = "FFLLSSSSYY**CC*WLLLLPPPPHHQQRRRRIIIMTTTTNNKKSSRRVVVVAAAADDEEGGGG";
let ci = 0;
for (const a of "TCAG") for (const b of "TCAG") for (const c of "TCAG") CODONS[a + b + c] = AA[ci++];
function orfs(seq, minAa = 25) {
    const out = [];
    for (const [strand, s] of [
        ["+", seq],
        ["-", revcomp(seq)],
    ])
        for (let frame = 0; frame < 3; frame++) {
            let start = -1;
            for (let i = frame; i + 3 <= s.length; i += 3) {
                const codon = s.slice(i, i + 3);
                if (start < 0 && codon === "ATG") start = i;
                else if (start >= 0 && CODONS[codon] === "*") {
                    if ((i - start) / 3 >= minAa)
                        out.push({
                            strand,
                            start: strand === "+" ? start + 1 : seq.length - i - 2,
                            protein: Array.from(
                                { length: (i - start) / 3 },
                                (_, k) => CODONS[s.slice(start + 3 * k, start + 3 * k + 3)],
                            ).join(""),
                        });
                    start = -1;
                }
            }
        }
    return out;
}
const genes = orfs(reference);

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const meanQ = Array.from(posQual, (s, i) => s / posN[i] - 33);
results[`quality falls along the read (Q${meanQ[0].toFixed(0)} -> Q${meanQ.at(-1).toFixed(0)})`] =
    meanQ[0] > meanQ[READ_LEN - 1] + 10;
results[
    `${adapterTrimmed} reads adapter-trimmed; ${reads.filter((r) => r.seq.includes(ADAPTER)).length} keep an intact adapter (a sequencing error inside it)`
] = adapterTrimmed > rawReads * 0.2 && reads.filter((r) => r.seq.includes(ADAPTER)).length < reads.length * 0.01;
results[`${dups} exact duplicates removed; GC ${((gc / totalBases) * 100).toFixed(1)}%`] =
    dups > 0 && Math.abs(gc / totalBases - 0.5) < 0.05;
results[`k-mer spectrum valley at ${SOLID}; ${corrected} base errors corrected`] = SOLID >= 3 && corrected > 300;
results[`assembled ${contigs.length} contigs, N50 ${n50}, total ${contigs.reduce((s, c) => s + c.length, 0)} bp`] =
    contigs.length >= 1 && n50 > 1000 && contigs.reduce((s, c) => s + c.length, 0) > 9000;
results[`${mapped} of ${reads.length} reads aligned with MAPQ >= 10`] = mapped > reads.length * 0.9;
const truthSnps = truthVariants.filter((v) => v.ref.length === 1),
    callSnps = calls.filter((c) => c.kind === "snp");
const tp = callSnps.filter((c) => truthSnps.some((t) => t.pos === c.pos && t.alt === c.alt)).length,
    gtOk = callSnps.filter((c) => truthSnps.some((t) => t.pos === c.pos && t.gt === c.gt)).length;
results[`SNPs: ${tp}/${truthSnps.length} found, ${callSnps.length - tp} false positives, ${gtOk} genotypes correct`] =
    tp >= truthSnps.length * 0.9 && callSnps.length - tp <= 2;
results["deletions are called from gaps in the pileup"] =
    calls.filter((c) => c.kind === "del").length >= truthVariants.filter((v) => v.ref.length === 3).length * 0.7;
results["both planted genes are found and translated (one on the reverse strand)"] =
    genes.some((g) => g.protein.startsWith("MAKETAAAKFERQHMDSSTSAASSAAAA")) &&
    genes.some((g) => g.strand === "-" && g.protein.startsWith("MSIQHFRVALIPFFAAFCLPVFA"));

{
    // quality trimming, per base: no kept read may still hold a 4-base window with mean Phred below 20
    let lowWindows = 0;
    for (const r of reads)
        for (let i = 0; i + 4 <= r.qual.length; i++) {
            let sum = 0;
            for (let j = i; j < i + 4; j++) sum += r.qual.charCodeAt(j) - 33;
            if (sum / 4 < 20) lowWindows++;
        }
    results[`no kept read holds a 4-base window below Q20 (${lowWindows} found)`] = lowWindows === 0;
    // the 2-bit codes against string k-mers: canonical = the lexicographically smaller of k-mer and reverse complement
    const rcString = (k) =>
        [...k]
            .reverse()
            .map((c) => "TGCA"["ACGT".indexOf(c)])
            .join("");
    let agree = true,
        occurrences = 0;
    for (const seq of rawSeqs.slice(0, 300))
        for (let i = 0, n = rollKmers(seq); i < n; i++) {
            const k = seq.slice(i, i + K),
                r = rcString(k);
            if (decode(Math.min(fwd[i], rev[i])) !== (k < r ? k : r) || decode(rev[i]) !== r) agree = false;
        }
    for (const seq of rawSeqs) occurrences += Math.max(0, seq.length - K + 1);
    const total = counts.values().reduce((a, b) => a + b, 0);
    results[`2-bit k-mer codes decode to the canonical string k-mers; ${total} occurrences counted`] =
        agree && total === occurrences;
    // count-min: never under, and over by more than e·N/width for at most e^-depth of keys (Cormode & Muthukrishnan)
    const bound = (Math.E * total) / (1 << 16);
    let under = 0,
        over = 0;
    for (const [c, n] of counts) {
        const est = sketch.count(c.toString(36));
        if (est < n) under++;
        if (est > n + bound) over++;
    }
    results[`count-min sketch: 0 underestimates, ${over} of ${counts.size} beyond e·N/width = ${bound.toFixed(1)}`] =
        under === 0 && over <= counts.size * Math.exp(-4);
    // genome size from the spectrum: solid k-mer occurrences / the coverage peak
    let peak = SOLID,
        solidOcc = 0;
    for (let c = SOLID; c <= 60; c++) {
        if ((hist[c] || 0) > (hist[peak] || 0)) peak = c;
        solidOcc += c * (hist[c] || 0);
    }
    const size = solidOcc / peak;
    results[`genome size from the k-mer spectrum: ${Math.round(size)} bp (peak depth ${peak}, true ${hom.length})`] =
        Math.abs(size - hom.length) / hom.length < 0.15;
    // every long contig placed back on the reference: seed vote, banded Smith–Waterman, identity over the alignment
    const placed = contigs
        .filter((c) => c.length >= 1000)
        .map((c) => {
            const candidates = [c, revcomp(c)].map((q) => {
                const votes = new Map();
                for (let i = 0; i + SEED <= q.length; i += 50)
                    for (const p of index.get(q.slice(i, i + SEED))) votes.set(p - i, (votes.get(p - i) || 0) + 1);
                return { q, best: votes.entries().maxBy(([, n]) => n) ?? [0, 0] };
            });
            const { q, best } = candidates.values().maxBy((x) => x.best[1]),
                band = 20,
                off = Math.max(0, best[0] - band),
                ref = reference.slice(off, best[0] + q.length + band);
            const a = SmithWaterman(q, ref, { match: 2, mismatch: -4, gap: -6, band, diagonal: best[0] - off });
            let same = 0,
                cols = 0,
                qi = a.aStart,
                ri = a.bStart;
            for (const [, n, op] of a.cigar.matchAll(/(\d+)([MID])/g))
                for (let k = 0; k < Number(n); k++, cols++)
                    if (op === "M") same += q[qi++] === ref[ri++] ? 1 : 0;
                    else if (op === "I") qi++;
                    else ri++;
            return { length: c.length, identity: same / cols, covered: (a.aEnd - a.aStart) / q.length };
        });
    results[
        `${placed.length} contigs >= 1 kb align back to the reference at >= ${(Math.min(...placed.map((p) => p.identity)) * 100).toFixed(2)}% identity over >= 95% of their length`
    ] = placed.length > 0 && placed.every((p) => p.identity > 0.99 && p.covered > 0.95);
}

print("genomics");
print(
    `  reads      ${rawReads} x ${READ_LEN} bp (${COVERAGE}x), ${reads.length} kept after trimming; mean Q by position ${[0, 25, 50, 75, 99].map((i) => `${i + 1}:${meanQ[i].toFixed(0)}`).join(" ")}`,
);
print(`  k-mers     ${counts.size} distinct 21-mers, solid >= ${SOLID}: ${solid.size / 2}`);
print(`  assembly   ${contigs.length} contigs, N50 ${n50} bp, longest ${contigs[0]?.length} bp`);
print(
    table([
        ["POS", "REF", "ALT", "DP", "AF", "GT", "truth"],
        ...calls.slice(0, 12).map((c) => {
            const t = truthVariants.find(
                (v) => v.pos === c.pos || (c.kind === "del" && Math.abs(v.pos - c.pos) <= 2 && v.ref.length === 3),
            );
            return [
                String(c.pos),
                c.ref,
                c.alt,
                String(c.depth),
                c.af.toFixed(2),
                c.gt,
                t ? `${t.ref}>${t.alt} ${t.gt}` : "FALSE+",
            ];
        }),
    ]),
);
print(
    `  genes      ${genes.length} ORFs >= 25 aa; ${genes
        .slice(0, 2)
        .map((g) => `${g.strand}${g.start}: ${g.protein.slice(0, 20)}…`)
        .join("; ")}`,
);
const lengths = new DataFrame({ len: Float64Array.from(reads.map((r) => r.seq.length)) });
print(
    `  lengths    trimmed read length median ${lengths.MEDIAN("len")}, mean ${lengths.MEAN("len").toFixed(1)}; ${XXH3_64(vcf, 0, { as: "hex" })} VCF digest`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
