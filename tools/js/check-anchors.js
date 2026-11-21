// flags: --std
// tools/js/check-anchors.js: the port of tools/check-anchors.py (deleted); its output was proven identical, apart from naming itself
// Check that every in-page `](#anchor)` link in a markdown file resolves to
// a heading anchor in the SAME file.
//
// Usage: ./dynajs --std tools/js/check-anchors.js FILE.md [FILE.md ...]
// Prints "<file>: N anchors, M broken" and file:line for each broken link;
// exit 1 if any, 0 if clean. GitHub slugging: lowercase, punctuation dropped
// EXCEPT hyphen and underscore, spaces to hyphens, duplicates get -2, -3...
// (see slug()) -- a checker that strips `_` reports io_uring anchors as broken
// and invites a "fix" that breaks a working link. Headings and links inside
// fenced code blocks are ignored.
import * as std from "std";
import { readText, pyStrip, Out } from "./pyshim.js";

const PUNCT = /[`*\[\]()<>.,:;!?'"\/\\|+=~^{}$@#%&]/g;
function slug(t) {
    t = pyStrip(t).toLowerCase();
    t = t.replace(PUNCT, "");
    t = t.replace(/[–—]/g, "");
    return t.replaceAll(" ", "-");
}

const o = new Out();
let badTotal = 0;
for (const p of scriptArgs.slice(1)) {
    const L = readText(p).split("\n");
    const seen = new Map();
    const anchors = new Set();
    let fence = false;
    for (const ln of L) {
        if (pyStrip(ln).startsWith("```")) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        const m = /^(#{1,6}) +([^\n]*)$/.exec(ln);
        if (!m) continue;
        const a = slug(m[2]);
        const n = seen.get(a) || 0;
        seen.set(a, n + 1);
        anchors.add(n === 0 ? a : `${a}-${n}`);
    }
    const bad = [];
    L.forEach((ln, i) => {
        for (const m of ln.matchAll(/\]\(#([^)]+)\)/g)) if (!anchors.has(m[1])) bad.push([i + 1, m[1]]);
    });
    badTotal += bad.length;
    o.print(`${p}: ${anchors.size} anchors, ${bad.length} broken`);
    for (const [i, t] of bad) o.print(`    ${p}:${i}  #${t}`);
}
o.flush();
std.exit(badTotal ? 1 : 0);
