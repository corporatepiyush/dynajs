// 37 · Text diff and review tool — unified diffs, word-level highlights and a change summary.
//
// WHAT IT SHOWS
//   - dyna:matcher DiffLines: the line-level edit script behind every code review
//   - grouping changes into hunks with context, the unified format `patch` understands
//   - DiffWords inside a changed line, to show WHAT changed rather than only that it did
//   - a similarity score for "is this a rewrite or a tweak"
//
// RUN      dynajs examples/apps/37-text-diff-review.js [old-file new-file]

import { DiffLines, DiffWords, DiceCoefficient } from "dyna:matcher";
import { Path, readFile } from "dyna:file";
import { StyleText } from "dyna:cli";

const CONTEXT = 2;                 // unchanged lines shown around each change

// Turn the edit script into numbered line records.
function lineRecords(oldText, newText) {
    const records = [];
    let oldNo = 1, newNo = 1;
    for (const hunk of DiffLines(oldText, newText)) {
        // A hunk's text holds one or more whole lines.
        const lines = hunk.text.endsWith("\n") ? hunk.text.slice(0, -1).split("\n") : hunk.text.split("\n");
        for (const text of lines) {
            if (hunk.op === 0) records.push({ op: " ", text, oldNo: oldNo++, newNo: newNo++ });
            else if (hunk.op < 0) records.push({ op: "-", text, oldNo: oldNo++ });
            else records.push({ op: "+", text, newNo: newNo++ });
        }
    }
    return records;
}

// Group records into hunks: each run of changes plus CONTEXT lines either
// side, merging hunks whose context would overlap.
function hunks(records) {
    const keep = new Uint8Array(records.length);
    records.forEach((r, i) => {
        if (r.op === " ") return;
        for (let j = Math.max(0, i - CONTEXT); j <= Math.min(records.length - 1, i + CONTEXT); j++) keep[j] = 1;
    });
    const out = [];
    for (let i = 0; i < records.length; i++) {
        if (!keep[i]) continue;
        const start = i;
        while (i + 1 < records.length && keep[i + 1]) i++;
        out.push(records.slice(start, i + 1));
    }
    return out;
}

function unified(oldName, newName, oldText, newText) {
    const lines = [`--- ${oldName}`, `+++ ${newName}`];
    for (const hunk of hunks(lineRecords(oldText, newText))) {
        const olds = hunk.filter((r) => r.op !== "+"), news = hunk.filter((r) => r.op !== "-");
        const span = (list, key) => `${list.length ? list[0][key] : 0},${list.length}`;
        lines.push(`@@ -${span(olds, "oldNo")} +${span(news, "newNo")} @@`);
        for (const r of hunk) lines.push(r.op + r.text);
    }
    return lines.join("\n") + "\n";
}

// For a removed line directly followed by an added one, mark the words that differ.
function wordHighlights(oldLine, newLine) {
    return DiffWords(oldLine, newLine).map((h) =>
        h.op === 0 ? h.text : h.op < 0 ? `[-${h.text}-]` : `{+${h.text}+}`).join("");
}

function summarize(oldText, newText) {
    const records = lineRecords(oldText, newText);
    const added = records.filter((r) => r.op === "+").length, removed = records.filter((r) => r.op === "-").length;
    return { added, removed, unchanged: records.length - added - removed, similarity: DiceCoefficient(oldText, newText) };
}

// ---- command line / self-test ---------------------------------------------
const args = scriptArgs.slice(1);
if (args.length === 2) {
    const [a, b] = args.map((f) => readFile(new Path(f)));
    const colour = (line) => line.startsWith("+") ? StyleText("green", line) : line.startsWith("-") ? StyleText("red", line)
                           : line.startsWith("@@") ? StyleText("cyan", line) : line;
    console.log(unified(args[0], args[1], a, b).split("\n").map(colour).join("\n"));
    const s = summarize(a, b);
    console.log(`${s.added} added, ${s.removed} removed, ${(s.similarity * 100).toFixed(0)}% similar`);
} else {
    const before = ["server:", "  host: 0.0.0.0", "  port: 8080", "  workers: 4", "", "logging:", "  level: info",
                    "  format: json", "", "cache:", "  ttl: 300", "  size: 1000", ""].join("\n");
    const after  = ["server:", "  host: 0.0.0.0", "  port: 8443", "  workers: 4", "  tls: true", "", "logging:", "  level: info",
                    "  format: json", "", "cache:", "  size: 1000", ""].join("\n");
    const patch = unified("config.yaml", "config.yaml", before, after);
    console.log(patch);
    console.log("word level:", wordHighlights("  port: 8080", "  port: 8443"));

    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const s = summarize(before, after);
    check(s.added === 2 && s.removed === 2, `two lines added, two removed (${s.added}/${s.removed})`);
    check(patch.includes("-  port: 8080\n+  port: 8443\n"), "a changed line appears as a removal and an addition");
    check(patch.includes("+  tls: true") && patch.includes("-  ttl: 300"), "pure additions and deletions are shown");
    check((patch.match(/^@@/gm) ?? []).length === 2, "distant changes form separate hunks");
    check(!patch.includes("  format: json"), "lines far from any change are left out");
    check(wordHighlights("  port: 8080", "  port: 8443") === "  port: [-8080-]{+8443+}", "word diff isolates the changed token");
    check(s.similarity > 0.8 && summarize(before, "something else entirely").similarity < 0.3, "similarity separates tweaks from rewrites");
    check(summarize(before, before).added === 0 && unified("a", "a", before, before) === "--- a\n+++ a\n", "identical inputs produce an empty diff");

    // Applying the patch by hand must reproduce the new text: the real test of a diff.
    const rebuilt = lineRecords(before, after).filter((r) => r.op !== "-").map((r) => r.text).join("\n");
    check(rebuilt === after.replace(/\n$/, ""), "the edit script rebuilds the new file exactly");
    console.log("self-test passed");
}
