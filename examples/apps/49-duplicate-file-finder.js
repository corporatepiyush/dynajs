// 49 · Duplicate file finder — finds identical files cheaply by narrowing with size, then a sample, then a full hash.
//
// WHAT IT SHOWS
//   - a three-stage filter that reads as little as possible: most files are ruled out by size alone
//   - dyna:file FileReader.readInto: reading just the first block of a large file
//   - dyna:hash Hasher: streaming a digest through one reused buffer, so memory stays flat
//   - reporting reclaimable space, and choosing which copy to keep by a stated rule
//
// RUN      dynajs examples/apps/49-duplicate-file-finder.js [directory]
//          Without an argument it builds a sample tree and checks the result.

import { Hasher, XXH3_64 } from "dyna:hash";
import { Path, FileReader, glob, stat, writeFile, makeDir, makeTempDir, removeAll } from "dyna:file";

const SAMPLE = 4096;                         // bytes read in the cheap second stage
const BLOCK = 256 * 1024;

// Full-content digest without loading the file: one buffer, reused per block.
function digest(path, buffer) {
    const hasher = new Hasher("sha256");
    const reader = new FileReader(path);
    try {
        for (let n; (n = reader.readInto(buffer)) > 0; ) hasher.update(buffer.subarray(0, n));
        return hasher.digestHex();
    } finally {
        reader.close();
        hasher.close();
    }
}

// A fast non-cryptographic hash of the first block. Good enough to separate
// files that merely share a size; the SHA-256 stage has the final word.
function sample(path, buffer) {
    const reader = new FileReader(path);
    try {
        const n = reader.readInto(buffer.subarray(0, SAMPLE));
        return XXH3_64(buffer.subarray(0, n));
    } finally { reader.close(); }
}

function findDuplicates(root) {
    const stats = { files: 0, bytes: 0, sampled: 0, hashed: 0 };
    const buffer = new Uint8Array(BLOCK);

    // Stage 1: group by size. A file with a unique size cannot have a twin.
    const bySize = new Map();
    for (const rel of glob("**", { cwd: root })) {
        const path = root.join(String(rel));
        const info = stat(path);
        if (!info.isFile || info.size === 0) continue;       // empty files are all "equal" and not interesting
        stats.files++; stats.bytes += info.size;
        const entry = { path, rel: String(rel), size: info.size, mtimeMs: info.mtimeMs };
        bySize.set(info.size, [...(bySize.get(info.size) ?? []), entry]);
    }

    const groups = [];
    for (const sameSize of bySize.values()) {
        if (sameSize.length < 2) continue;

        // Stage 2: among equal sizes, compare a hash of the first 4 KiB.
        const bySample = new Map();
        for (const f of sameSize) {
            stats.sampled++;
            const key = sample(f.path, buffer);
            bySample.set(key, [...(bySample.get(key) ?? []), f]);
        }
        for (const sameSample of bySample.values()) {
            if (sameSample.length < 2) continue;

            // Stage 3: only now read whole files.
            const byDigest = new Map();
            for (const f of sameSample) {
                stats.hashed++;
                const key = digest(f.path, buffer);
                byDigest.set(key, [...(byDigest.get(key) ?? []), f]);
            }
            for (const [sha256, twins] of byDigest) {
                if (twins.length < 2) continue;
                // Keep the oldest copy (ties broken by path): the likeliest original.
                twins.sort((a, b) => a.mtimeMs - b.mtimeMs || a.rel.localeCompare(b.rel));
                groups.push({ sha256, size: twins[0].size, keep: twins[0].rel, duplicates: twins.slice(1).map((t) => t.rel) });
            }
        }
    }
    groups.sort((a, b) => b.size * b.duplicates.length - a.size * a.duplicates.length);
    const reclaimable = groups.reduce((n, g) => n + g.size * g.duplicates.length, 0);
    return { groups, reclaimable, stats };
}

function report({ groups, reclaimable, stats }) {
    console.log(`${stats.files} files, ${stats.bytes} bytes; sampled ${stats.sampled}, fully hashed ${stats.hashed}`);
    for (const g of groups)
        console.log(`  ${g.size} bytes x${g.duplicates.length + 1}  keep ${g.keep}  remove ${g.duplicates.join(", ")}`);
    console.log(`${groups.length} duplicate group(s), ${reclaimable} bytes reclaimable`);
}

// ---- command line / self-test ---------------------------------------------
if (scriptArgs.length > 1) {
    report(findDuplicates(new Path(scriptArgs[1])));
} else {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const root = makeTempDir("dupes");
    makeDir(root.join("photos", "2025"), { recursive: true });
    makeDir(root.join("backup"));

    const photo = new Uint8Array(300000).map((_, i) => (i * 31) & 255);          // spans two read blocks
    const sameStartDifferentEnd = photo.slice(); sameStartDifferentEnd[299999] ^= 1;
    const sameSizeDifferentStart = photo.slice(); sameSizeDifferentStart[0] ^= 1;

    writeFile(root.join("photos", "2025", "beach.raw"), photo);
    writeFile(root.join("backup", "beach-copy.raw"), photo);
    writeFile(root.join("backup", "beach-copy-2.raw"), photo);
    writeFile(root.join("photos", "edited.raw"), sameStartDifferentEnd);          // survives the sample, fails the hash
    writeFile(root.join("photos", "other.raw"), sameSizeDifferentStart);          // fails the sample
    writeFile(root.join("notes.txt"), "meeting at ten\n");
    writeFile(root.join("backup", "notes.txt"), "meeting at ten\n");
    writeFile(root.join("unique.txt"), "nothing else is this long, at all\n");   // unique size: never opened
    writeFile(root.join("empty.a"), ""); writeFile(root.join("empty.b"), "");

    const result = findDuplicates(root);
    report(result);
    check(result.groups.length === 2, "two groups of duplicates");
    const [big, small] = result.groups;
    check(big.size === 300000 && big.duplicates.length === 2, "three identical photos form one group");
    check(!big.duplicates.includes("photos/edited.raw") && big.keep !== "photos/edited.raw", "a file differing in its last byte is not a duplicate");
    check(small.size === 15 && small.duplicates.length === 1, "the two notes are paired");
    check(result.reclaimable === 2 * 300000 + 15, "reclaimable space counts every extra copy: " + result.reclaimable);
    check(result.stats.files === 8, "empty files are skipped");
    check(result.stats.sampled === 7 && result.stats.hashed === 6, `the funnel narrows: ${result.stats.sampled} sampled, ${result.stats.hashed} hashed`);
    console.log("self-test passed");
    removeAll(root);
}
