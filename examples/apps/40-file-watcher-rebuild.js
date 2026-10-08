// 40 · Watch and rebuild — reruns a build step when source files change, debounced and filtered.
//
// WHAT IT SHOWS
//   - dyna:file Watcher: kernel file events (kqueue/inotify), recursive, with an ignore list
//   - dyna:async debounce: an editor's burst of saves becomes one rebuild
//   - never overlapping builds: changes that arrive mid-build queue exactly one follow-up
//   - an incremental build keyed on content hashes, so touching a file without changing it is free
//
// RUN      dynajs examples/apps/40-file-watcher-rebuild.js [src-dir] [out-dir]
//          With arguments it watches until interrupted; without, it edits files itself and exits.

import { Watcher, Path, glob, readFile, writeFile, makeDir, makeTempDir, removeAll, remove, exists } from "dyna:file";
import { debounce } from "dyna:async";
import { SHA256Hex } from "dyna:hash";
import { MarkdownToHTML } from "dyna:html";

class Builder {
    constructor(srcDir, outDir) {
        this.srcDir = srcDir; this.outDir = outDir;
        this.digests = new Map();                 // source path -> content hash at last build
        this.builds = 0; this.running = false; this.dirty = false;
    }

    // One pass: convert every changed .md to .html, delete outputs whose source vanished.
    buildOnce() {
        const sources = new Set(glob("*.md", { cwd: this.srcDir }).map(String));
        const changed = [];
        for (const rel of sources) {
            const text = readFile(this.srcDir.join(rel));
            const digest = SHA256Hex(text);
            if (this.digests.get(rel) === digest) continue;        // saved, but identical
            this.digests.set(rel, digest);
            writeFile(this.outDir.join(rel.replace(/\.md$/, ".html")), MarkdownToHTML(text));
            changed.push(rel);
        }
        for (const rel of [...this.digests.keys()]) {
            if (sources.has(rel)) continue;
            this.digests.delete(rel);
            const out = this.outDir.join(rel.replace(/\.md$/, ".html"));
            if (exists(out)) remove(out);
            changed.push(rel + " (removed)");
        }
        this.builds++;
        return changed;
    }

    // Called on every (debounced) change. If a build is in flight, remember
    // that another is needed instead of starting a second one beside it.
    async request(onDone) {
        if (this.running) { this.dirty = true; return; }
        this.running = true;
        do {
            this.dirty = false;
            const changed = this.buildOnce();
            await sleep(0);                        // a real build would be asynchronous work
            onDone?.(changed);
        } while (this.dirty);
        this.running = false;
    }
}

function watch(srcDir, builder, onBuild) {
    const watcher = new Watcher(srcDir, { recursive: true, debounceMs: 20, ignore: ["*.swp", "*~", ".git"] });
    // The watcher coalesces per file; this debounce coalesces across files, so
    // "save all" in an editor triggers one build, not one per file.
    const trigger = debounce(() => builder.request(onBuild), 40);
    watcher.start((event) => { if (event.path.endsWith(".md")) trigger(); });
    return watcher;
}

// ---- command line / self-test ---------------------------------------------
const args = scriptArgs.slice(1);
if (args.length >= 2) {
    const [srcDir, outDir] = args.map((a) => new Path(a));
    makeDir(outDir, { recursive: true });
    const builder = new Builder(srcDir, outDir);
    console.log("initial build:", builder.buildOnce().length, "files");
    watch(srcDir, builder, (changed) => changed.length && console.log(new Date().toISOString(), "rebuilt", changed.join(", ")));
    console.log("watching", String(srcDir), "(Ctrl-C to stop)");
} else {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const work = makeTempDir("watch");
    const src = work.join("src"), out = work.join("out");
    makeDir(src); makeDir(out);
    writeFile(src.join("index.md"), "# Home\n");
    writeFile(src.join("about.md"), "# About\n");

    const builder = new Builder(src, out);
    check(builder.buildOnce().length === 2 && exists(out.join("index.html")), "the initial build converts everything");

    const rebuilds = [];
    const watcher = watch(src, builder, (changed) => rebuilds.push(changed));
    await sleep(100);                                             // let the watch arm
    const settle = async (n) => { for (let i = 0; i < 100 && rebuilds.length < n; i++) await sleep(20); };

    // A burst of five saves to two files becomes one rebuild.
    for (let i = 0; i < 5; i++) { writeFile(src.join("index.md"), `# Home v${i}\n`); writeFile(src.join("about.md"), `# About v${i}\n`); }
    await settle(1);
    await sleep(150);
    check(rebuilds.length === 1 && rebuilds[0].length === 2, "a burst of saves triggers one rebuild of both files: " + JSON.stringify(rebuilds));
    check(readFile(out.join("index.html")).includes("Home v4"), "the output reflects the last save");

    // Non-source files and ignored patterns do not trigger anything.
    writeFile(src.join("notes.txt"), "x"); writeFile(src.join("index.md.swp"), "x");
    await sleep(200);
    check(rebuilds.length === 1, "irrelevant files are ignored");

    // Rewriting a file with identical content rebuilds nothing.
    writeFile(src.join("about.md"), "# About v4\n");
    await settle(2);
    check(rebuilds[1]?.length === 0, "an unchanged file is detected by its hash and skipped");

    // New and deleted sources are picked up.
    writeFile(src.join("contact.md"), "# Contact\n");
    remove(src.join("about.md"));
    await settle(3);
    await sleep(150);
    const last = rebuilds.flat().join(",");
    check(exists(out.join("contact.html")) && !exists(out.join("about.html")), "added files appear and removed files disappear: " + last);

    watcher.close();
    console.log(`self-test passed: ${builder.builds} builds for ${rebuilds.length} batches of changes`);
    removeAll(work);
}
