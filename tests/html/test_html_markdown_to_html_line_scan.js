// covers: c:src/core/markdown/markdown.c
// MarkdownToHTML finds each line end with memchr('\n') (the block parser
// re-reads lines several times while it classifies them) instead of stepping
// byte by byte; a CR before the LF is dropped, and the last line may have no
// newline. Modperf round 2026-11-16. Oracle: a JS renderer for the subset
// generated here: paragraphs of plain words (lines joined by "\n"), fenced
// code (each line escaped and ended by "\n"), and ATX headings; blocks are
// separated by blank lines; line ends are LF or CRLF at random; line lengths
// run from 0 to past 300 bytes.
import { suite } from "../lib/kit.js";
import { MarkdownToHTML } from "dyna:html";

const t = suite("test_html_markdown_to_html_line_scan");

const esc = (s) => s.replace(/[<>&]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;" })[c]);
let seed = 0x3141;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
const WORDS = ["alpha", "beta", "x", "Gamma9", "dé", "名字", "zz"];
const line = (max) => {
    const k = 1 + rnd(max);
    const w = [];
    for (let i = 0; i < k; i++) w.push(WORDS[rnd(WORDS.length)]);
    return w.join(" ");
};
const CODE = ["x < 1 && y > 2", "", "  indented", "\tTab", "a&b", "<tag>", "名"];

function gen() {
    const blocks = [];
    for (let b = rnd(6); b >= 0; b--) {
        const kind = rnd(3);
        if (kind === 0) {
            const lines = [];
            for (let i = 1 + rnd(4); i > 0; i--) lines.push(line(rnd(5) ? 6 : 60));
            blocks.push({ src: lines, html: "<p>" + lines.join("\n") + "</p>\n" });
        } else if (kind === 1) {
            const lines = [];
            for (let i = rnd(4); i > 0; i--) lines.push(CODE[rnd(CODE.length)] + (rnd(4) ? "" : " " + line(50)));
            blocks.push({ src: ["```"].concat(lines, ["```"]), html: "<pre><code>" + lines.map((l) => esc(l) + "\n").join("") + "</code></pre>\n" });
        } else {
            const w = line(3);
            blocks.push({ src: ["# " + w], html: "<h1>" + w + "</h1>\n" });
        }
    }
    return blocks;
}

t.test("generated documents with LF and CRLF line ends", ({ eq }) => {
    for (let r = 0; r < 500; r++) {
        const blocks = gen();
        const nl = () => (rnd(2) ? "\n" : "\r\n");
        let src = "";
        blocks.forEach((b, i) => {
            src += b.src.join(nl()) + (i + 1 < blocks.length ? nl() + nl() : rnd(2) ? nl() : "");
        });
        eq(MarkdownToHTML(src), blocks.map((b) => b.html).join(""), "doc " + r + " " + JSON.stringify(src.slice(0, 80)));
    }
});

t.test("empty input, blank lines only, and a last line without a newline", ({ eq }) => {
    eq(MarkdownToHTML(""), "", "empty");
    eq(MarkdownToHTML("\n\r\n\n"), "", "blank lines");
    eq(MarkdownToHTML("one"), "<p>one</p>\n", "no newline");
    eq(MarkdownToHTML("one\r"), "<p>one</p>\n", "a lone CR at the end");
    eq(MarkdownToHTML("a\nb\r\nc"), "<p>a\nb\nc</p>\n", "mixed");
    const long = "w".repeat(5000);
    eq(MarkdownToHTML(long + "\n" + long), "<p>" + long + "\n" + long + "</p>\n", "5000-byte lines");
});

await t.run();
