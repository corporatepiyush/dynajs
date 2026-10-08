// 19 · Backup archiver — packs a directory into a compressed, checksummed archive and verifies a restore.
//
// WHAT IT SHOWS
//   - dyna:file glob + stat: walking a tree with an ignore list
//   - dyna:compress TarPack + zstd: one portable artifact
//   - a manifest of SHA-256 digests inside the archive, so a restore can prove itself
//   - restore-side safety: sizes capped and unsafe member names refused by the library
//
// RUN      dynajs examples/apps/19-backup-archiver.js backup  <dir> <archive.tar.zst>
//          dynajs examples/apps/19-backup-archiver.js restore <archive.tar.zst> <dir>
//          With no arguments it backs up and restores a sample tree.

import { TarPack, TarExtract, zstd, unzstd } from "dyna:compress";
import { SHA256Hex } from "dyna:hash";
import { Path, Glob, glob, stat, readBytes, writeFile, makeDir, makeTempDir, removeAll, exists } from "dyna:file";

const MANIFEST = ".backup-manifest.json";
// Glob matches the whole path lexically and its `*` crosses "/", so "*.tmp"
// means "ends in .tmp at any depth" and "node_modules/*" covers the subtree.
const IGNORE = ["node_modules/*", "*/node_modules/*", "*.tmp", ".git/*"].map((p) => new Glob(p));
const MAX_RESTORE_BYTES = 512 * 1024 * 1024;     // refuse archives that expand past this

function backup(sourceDir, archivePath) {
    const entries = [];
    const manifest = { createdAt: new Date().toISOString(), files: {} };
    // glob returns SORTED paths, so two backups of the same tree are byte-identical
    // apart from the timestamp: easy to diff, friendly to deduplicating storage.
    // With { cwd } the results are relative to it, which is the member name we want.
    for (const relPath of glob("**", { cwd: sourceDir })) {
        const rel = String(relPath);
        const path = sourceDir.join(rel);
        if (IGNORE.some((g) => g.matches(new Path(rel)))) continue;
        const info = stat(path);
        if (!info.isFile) continue;
        const data = readBytes(path);
        manifest.files[rel] = { sha256: SHA256Hex(data), size: info.size };
        entries.push({ name: rel, data, mode: info.mode & 0o777, mtime: Math.floor(info.mtimeMs / 1000) });
    }
    entries.push({ name: MANIFEST, data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) });
    const archive = zstd(TarPack(entries), { level: 9 });
    writeFile(archivePath, archive);
    const raw = entries.reduce((n, e) => n + e.data.length, 0);
    return { files: entries.length - 1, raw, packed: archive.length };
}

function restore(archivePath, targetDir) {
    // maxOutputBytes makes a decompression bomb a clean RangeError.
    const tar = unzstd(readBytes(archivePath), { maxOutputBytes: MAX_RESTORE_BYTES });
    // TarExtract refuses absolute names and ".." components, so a hostile
    // archive cannot write outside targetDir.
    const members = TarExtract(tar);
    const manifestEntry = members.find((m) => m.name === MANIFEST);
    if (!manifestEntry) throw new Error("archive has no manifest: not one of our backups");
    const manifest = JSON.parse(new TextDecoder().decode(manifestEntry.data));

    // Verify everything BEFORE writing anything: a corrupt backup should not
    // leave a half-restored directory behind.
    for (const m of members) {
        if (m.name === MANIFEST) continue;
        const expected = manifest.files[m.name];
        if (!expected) throw new Error("unexpected member: " + m.name);
        if (SHA256Hex(m.data) !== expected.sha256) throw new Error("checksum mismatch: " + m.name);
    }
    const missing = Object.keys(manifest.files).filter((f) => !members.some((m) => m.name === f));
    if (missing.length) throw new Error("archive is missing: " + missing.join(", "));

    for (const m of members) {
        if (m.name === MANIFEST) continue;
        const dest = targetDir.join(m.name);
        makeDir(dest.dirname, { recursive: true });
        writeFile(dest, m.data);
    }
    return { files: members.length - 1, createdAt: manifest.createdAt };
}

// ---- command line ----------------------------------------------------------
const [command, a, b] = scriptArgs.slice(1);
if (command === "backup") {
    const r = backup(new Path(a), new Path(b));
    console.log(`backed up ${r.files} files, ${r.raw} -> ${r.packed} bytes`);
} else if (command === "restore") {
    const r = restore(new Path(a), new Path(b));
    console.log(`restored ${r.files} files from the backup taken ${r.createdAt}`);
} else {
    // ---- self-test ---------------------------------------------------------
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const work = makeTempDir("backup");
    const src = work.join("project");
    makeDir(src.join("src", "lib"), { recursive: true });
    makeDir(src.join("node_modules", "dep"), { recursive: true });
    writeFile(src.join("README.txt"), "hello\n".repeat(200));
    writeFile(src.join("src", "main.js"), "console.log('main');\n");
    writeFile(src.join("src", "lib", "util.js"), "export const id = (x) => x;\n");
    writeFile(src.join("scratch.tmp"), "ignore me");
    writeFile(src.join("node_modules", "dep", "index.js"), "ignored");

    const archive = work.join("project.tar.zst");
    const made = backup(src, archive);
    check(made.files === 3, "ignored paths are left out: " + made.files);
    check(made.packed < made.raw, "the archive is smaller than the tree");

    const out = work.join("restored");
    check(restore(archive, out).files === 3, "three files are restored");
    check(String.fromCharCode(...readBytes(out.join("src", "lib", "util.js"))).includes("export const id"), "nested content survives");
    check(!exists(out.join("scratch.tmp")) && !exists(out.join(MANIFEST)), "only real files are written");

    // Change one byte of one file's CONTENT inside the archive: restore must
    // refuse. (A flipped byte in a tar header's padding would alter nothing
    // the manifest protects, so the damage is aimed at real data.)
    const tar = unzstd(readBytes(archive));
    const needle = new TextEncoder().encode("console.log");
    let at = -1;
    for (let k = 0; k + needle.length <= tar.length && at < 0; k++)
        if (needle.every((b, n) => tar[k + n] === b)) at = k;
    tar[at] ^= 0x20;                                  // "console" -> "Console"
    writeFile(work.join("damaged.tar.zst"), zstd(tar));
    let refused = "";
    try { restore(work.join("damaged.tar.zst"), work.join("bad")); } catch (e) { refused = e.message; }
    check(refused === "checksum mismatch: src/main.js" && !exists(work.join("bad")), "a damaged archive is refused before any write: " + refused);
    console.log(`self-test passed: ${made.files} files, ${made.raw} -> ${made.packed} bytes; damage refused (${refused})`);
    removeAll(work);
}
