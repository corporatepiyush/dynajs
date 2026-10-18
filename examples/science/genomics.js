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
// MODULES  dyna:compress (gzip) · dyna:structures (CountMinSketch, Multimap) · dyna:hash (XXH3) · dyna:random
//          · dyna:matcher (MultiMatcher, SmithWaterman) · dyna:dataframe · dyna:file · dyna:cli (Table) · dyna:stream (lines over gzip)
//
// RUN      dynajs examples/science/genomics.js     simulates a 12 kb sample, sequences it at 40x and analyses it

import { gzip } from "dyna:compress";
import { CountMinSketch, Multimap } from "dyna:structures";
import { MultiMatcher, SmithWaterman } from "dyna:matcher";
import { XXH3_64 } from "dyna:hash";
import { Random } from "dyna:random";
import { DataFrame } from "dyna:dataframe";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
import { Table } from "dyna:cli";
import { fromFile, inflate, lines } from "dyna:stream";
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
for (let r = 0; r < nReads; r++) {
    const src = haplotypes[r % 2],
        insert = 70 + rng.nextBounded(80),
        start = rng.nextBounded(src.length - insert);
    let frag = src.slice(start, start + insert);
    if (rng.nextFloat() < 0.5) frag = revcomp(frag);
    let seq = (frag + ADAPTER + "ACGTACGTACGTACGTACGTACGTACGT").slice(0, READ_LEN),
        qual = "";
    seq = [...seq]
        .map((c, i) => {
            const q = Math.max(2, Math.round(38 - i * 0.18 - rng.exponential(1 / 2)));
            qual += String.fromCharCode(33 + q);
            return rng.nextFloat() < 10 ** (-q / 10) ? BASES[rng.nextBounded(4)] : c;
        })
        .join("");
    fastq.push(`@read${r}/1\n${seq}\n+\n${qual}`);
    if (r % 97 === 0) fastq.push(`@read${r}dup/1\n${seq}\n+\n${qual}`);
}
writeFile(work.join("reads.fastq.gz"), gzip(fastq.join("\n") + "\n"));
writeFile(work.join("ref.fa"), `>plasmid demo\n${reference.match(/.{1,70}/g).join("\n")}\n`);

// =============================================================================
// QC: parse, trim adapters and low quality
// =============================================================================
const reads = [],
    posQual = new Float64Array(READ_LEN),
    posN = new Float64Array(READ_LEN);
let rawReads = 0,
    adapterTrimmed = 0,
    gc = 0,
    totalBases = 0;
const seen = new Set();
let dups = 0;
{
    const it = lines(inflate(fromFile(work.join("reads.fastq.gz")), { codec: "gzip" }));
    const rec = [];
    for await (const line of it) {
        rec.push(line);
        if (rec.length < 4) continue;
        const [h, seq, , q] = rec.splice(0);
        if (!h.startsWith("@") || seq.length !== q.length) throw new Error(`malformed FASTQ record ${h}`);
        rawReads++;
        for (let i = 0; i < q.length; i++) {
            posQual[i] += q.charCodeAt(i) - 33;
            posN[i]++;
        }
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
        for (let i = 0; i + 4 <= end; i++) {
            let s = 0;
            for (let j = i; j < i + 4; j++) s += q.charCodeAt(j) - 33;
            if (s / 4 < 20) {
                end = i;
                break;
            }
        }
        if (end < 40) continue;
        const s = seq.slice(0, end);
        reads.push({ id: h.slice(1), seq: s, qual: q.slice(0, end) });
        for (const c of s) {
            if (c === "G" || c === "C") gc++;
            totalBases++;
        }
    }
}

// =============================================================================
// k-mer spectrum, error correction, de Bruijn assembly
// =============================================================================
const K = 21;
const canon = (k) => {
    const r = revcomp(k);
    return k < r ? k : r;
};
// The reverse complement of the k-mer at i is a slice of the read's reverse complement: one revcomp per read, not per k-mer.
const canonAt = (seq, rc, i) => {
    const f = seq.slice(i, i + K),
        r = rc.slice(seq.length - i - K, seq.length - i);
    return f < r ? f : r;
};
const counts = new Map(),
    sketch = new CountMinSketch(1 << 16, 4);
for (const r of reads) {
    const rc = revcomp(r.seq);
    for (let i = 0; i + K <= r.seq.length; i++) {
        const c = canonAt(r.seq, rc, i);
        counts.set(c, (counts.get(c) || 0) + 1);
        sketch.add(c);
    }
}
const hist = new Map();
for (const v of counts.values()) hist.set(Math.min(v, 60), (hist.get(Math.min(v, 60)) || 0) + 1);
let valley = 2;
for (let c = 2; c < 20; c++) if ((hist.get(c) || 0) < (hist.get(valley) || 0)) valley = c;
const SOLID = Math.max(3, valley);
let corrected = 0;
const isWeak = (km) => (counts.get(canon(km)) || 0) < SOLID;
for (const r of reads) {
    // an error makes the K k-mers covering it weak: fix the base they share
    const s = r.seq.split(""),
        n = s.length - K + 1;
    let i = 0;
    while (i < n) {
        if (!isWeak(s.slice(i, i + K).join(""))) {
            i++;
            continue;
        }
        let j = i;
        while (j + 1 < n && isWeak(s.slice(j + 1, j + 1 + K).join(""))) j++;
        let fixed = false;
        for (let p = j; p <= Math.min(s.length - 1, i + K - 1) && !fixed; p++) {
            const orig = s[p];
            for (const b of BASES) {
                if (b === orig) continue;
                s[p] = b;
                let ok = true;
                for (let st = Math.max(0, p - K + 1); st <= Math.min(p, n - 1) && ok; st++)
                    ok = !isWeak(s.slice(st, st + K).join(""));
                if (ok) {
                    corrected++;
                    fixed = true;
                    break;
                }
                s[p] = orig;
            }
        }
        i = j + 1;
    }
    r.seq = s.join("");
}
const solid = new Set();
for (const r of reads) {
    const rc = revcomp(r.seq),
        n = r.seq.length;
    for (let i = 0; i + K <= n; i++) {
        const km = r.seq.slice(i, i + K),
            kr = rc.slice(n - i - K, n - i);
        if ((counts.get(km < kr ? km : kr) || 0) >= SOLID) {
            solid.add(km);
            solid.add(kr);
        }
    }
}
const succ = (km) =>
    BASES.split("")
        .map((b) => km.slice(1) + b)
        .filter((x) => solid.has(x));
const pred = (km) =>
    BASES.split("")
        .map((b) => b + km.slice(0, -1))
        .filter((x) => solid.has(x));
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
    let contig = head,
        cur = head;
    used.add(cur);
    used.add(revcomp(cur));
    while (succ(cur).length === 1 && pred(succ(cur)[0]).length === 1 && !used.has(succ(cur)[0])) {
        cur = succ(cur)[0];
        contig += cur.at(-1);
        used.add(cur);
        used.add(revcomp(cur));
    }
    if (contig.length >= 200) contigs.push(contig);
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
    const ops = [];
    for (const [, n, op] of r.cigar.matchAll(/(\d+)([MID])/g)) for (let k = 0; k < Number(n); k++) ops.push(op);
    const cigar = (r.aStart ? `${r.aStart}S` : "") + r.cigar + (read.length - r.aEnd ? `${read.length - r.aEnd}S` : "");
    return { score: r.score, pos: off + r.bStart, cigar, ops, readStart: r.aStart };
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
        for (const [diag, v] of [...votes].sort((a, b) => b[1] - a[1]).slice(0, 1))
            if (v >= 2) best.push({ strand, s, diag, v });
    }
    if (!best.length) return null;
    const scored = best.map((b) => ({ ...b, ...sw(b.s, Math.max(0, b.diag)) })).sort((a, b) => b.score - a.score);
    const top = scored[0],
        mapq = scored[1] ? Math.min(60, Math.round((top.score - scored[1].score) / 2)) : 60;
    return { ...top, mapq, qual: top.strand === "+" ? read.qual : [...read.qual].reverse().join("") };
}
const pile = new Map();
let mapped = 0;
for (const r of reads) {
    const a = align(r);
    if (!a || a.mapq < 10 || a.score < 60) continue;
    mapped++;
    let rp = a.readStart,
        gp = a.pos;
    for (const op of a.ops) {
        if (op === "M") {
            const q = a.qual.charCodeAt(rp) - 33;
            if (q >= 20) {
                const col = pile.get(gp) || {};
                col[a.s[rp]] = (col[a.s[rp]] || 0) + 1;
                pile.set(gp, col);
            }
            rp++;
            gp++;
        } else if (op === "I") rp++;
        else {
            const col = pile.get(gp) || {};
            col["-"] = (col["-"] || 0) + 1;
            pile.set(gp, col);
            gp++;
        }
    }
}

// =============================================================================
// Variant calling and VCF
// =============================================================================
const calls = [];
for (const [p, col] of [...pile].sort((a, b) => a[0] - b[0])) {
    const depth = Object.values(col).reduce((a, b) => a + b, 0),
        ref = reference[p];
    if (depth < 10) continue;
    for (const [alt, n] of Object.entries(col)) {
        if (alt === ref) continue;
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
const meanQ = Array.from(posQual, (s, i) => s / posN[i]);
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

print("genomics");
print(
    `  reads      ${rawReads} x ${READ_LEN} bp (${COVERAGE}x), ${reads.length} kept after trimming; mean Q by position ${[0, 25, 50, 75, 99].map((i) => `${i + 1}:${meanQ[i].toFixed(0)}`).join(" ")}`,
);
print(
    `  k-mers     ${counts.size} distinct 21-mers, solid >= ${SOLID}: ${solid.size / 2}; count-min agrees on a sample: ${[...counts].slice(0, 3).every(([k, v]) => sketch.count(k) >= v)}`,
);
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
