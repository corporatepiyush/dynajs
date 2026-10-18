// Publishing pipeline — a technical book from Markdown chapters to a validated EPUB 3, a single-page HTML edition
// and plain text, with cross-references, footnotes, citations, a generated index, editorial checks and an
// edition-to-edition change report.
//
// WHAT IT IS
//   What a small press or a docs team runs on every manuscript commit:
//
//   manuscript   chapters in Markdown with YAML front matter (title, id, status), a bibliography in YAML,
//                a house style guide and an index term list
//   references   {#sec:id} anchors and [@sec:id] cross-references resolve to "Section 2.3"; [@knuth1984]
//                citations become numbered references with a bibliography in citation order; [^n] footnotes are
//                collected per chapter; an unresolved reference is an error with chapter and line
//   editorial    spelling against a dictionary (suggestions by edit distance), style rules (banned phrases,
//                passive constructions, sentence length), Flesch reading ease per chapter, and links checked
//                for scheme and well-formedness
//   index        index terms are found in the rendered text and listed with the sections they occur in
//   EPUB 3       a ZIP whose first entry is an UNCOMPRESSED `mimetype`, a container.xml, an OPF package with a
//                manifest and spine, an XHTML navigation document, and one XHTML file per chapter — then the
//                result is re-read and validated (entry order, every manifest item present, XML well-formed)
//   editions     a word-level diff between the previous and the current edition, summarised per chapter
//   spelling     suggestions use the SymSpell idea: every dictionary word's deletions (up to two characters)
//                are indexed once in a Multimap; an unknown word's own deletions name a handful of candidates,
//                and only those are scored with DamerauLevenshtein — not the whole dictionary per typo
//
// MODULES  dyna:html (MarkdownToHTML, HTMLParse, Selector, HTMLText, Template) · dyna:config (front matter)
//          · dyna:yaml · dyna:compress (ZipPack, ZipList, ZipRead) · dyna:xml (XmlWriter, XMLParse)
//          · dyna:matcher (DamerauLevenshtein, DiffWords, MultiMatcher) · dyna:uuid · dyna:hash · dyna:file
//          · dyna:structures (Multimap)
//
// RUN      dynajs examples/text/publishing-pipeline.js     builds the sample book twice (two editions)

import { MarkdownToHTML, HTMLParse, HTMLText, Selector, Template } from "dyna:html";
import { FrontMatter } from "dyna:config";
import * as YAML from "dyna:yaml";
import { ZipPack, ZipList, ZipRead, ZipReadAt } from "dyna:compress";
import { XmlWriter, XMLParse } from "dyna:xml";
import { DamerauLevenshtein, DiffWords, MultiMatcher } from "dyna:matcher";
import { v5 as uuid5, NAMESPACE_URL } from "dyna:uuid";
import { SHA256Hex } from "dyna:hash";
import { Multimap } from "dyna:structures";
import { makeTempDir, makeDir, writeFile, readFile, readDir, removeAll } from "dyna:file";

const work = makeTempDir("book");
const dec = new TextDecoder();

// =============================================================================
// The manuscript (two editions)
// =============================================================================
function writeManuscript(dir, edition) {
    makeDir(dir.join("chapters"), { recursive: true });
    writeFile(
        dir.join("book.yaml"),
        `title: Building Reliable Services\nauthor: R. Example\nlanguage: en\nedition: ${edition}\npublisher: Example Press\n`,
    );
    writeFile(
        dir.join("bibliography.yaml"),
        `
knuth1984: { author: "Donald E. Knuth", title: "Literate Programming", venue: "The Computer Journal 27(2)", year: 1984 }
lamport1978: { author: "Leslie Lamport", title: "Time, Clocks, and the Ordering of Events in a Distributed System", venue: "CACM 21(7)", year: 1978 }
dean2013: { author: "Jeffrey Dean and Luiz André Barroso", title: "The Tail at Scale", venue: "CACM 56(2)", year: 2013 }
brewer2000: { author: "Eric Brewer", title: "Towards Robust Distributed Systems", venue: "PODC keynote", year: 2000 }
`,
    );
    writeFile(
        dir.join("index-terms.yaml"),
        "- idempotency\n- tail latency\n- retry\n- backoff\n- logical clock\n- circuit breaker\n",
    );
    const ch = {
        "01-introduction.md": `---\nid: intro\ntitle: Introduction\nstatus: final\n---\n# Introduction {#sec:intro}\n\nReliable services are built, not hoped for. This book follows the idea of literate programming [@knuth1984]: code and explanation belong together.\n\nWe start with time and ordering in [@sec:clocks] and finish with failure handling in [@sec:retries].${edition > 1 ? " This edition adds a section on hedged requests." : ""}\n\n## How to read this book {#sec:howto}\n\nEach chapter ends with exercises.[^1] Examples are in JavaScript and run as written.\n\n[^1]: Solutions are online at https://example.com/solutions.\n`,
        "02-clocks.md": `---\nid: clocks\ntitle: Time and Order\nstatus: final\n---\n# Time and Order {#sec:clocks}\n\nPhysical clocks drift. A logical clock orders events without trusting wall time [@lamport1978]. Every message carries a counter, and the receiver advances its own counter past it.\n\n## Lamport timestamps {#sec:lamport}\n\nThe rule is short: increment before each event, and on receipt take the maximum. It is used in [@sec:retries] to order retries.\n\nIt is commonly believed that ordering requires synchronised clocks; it does not.[^1]\n\n[^1]: See also vector clocks, which capture concurrency explicitly.\n`,
        "03-retries.md": `---\nid: retries\ntitle: Retries and Tail Latency\nstatus: ${edition > 1 ? "final" : "draft"}\n---\n# Retries and Tail Latency {#sec:retries}\n\nAt scale, the slowest of many parallel calls decides the response time; this is tail latency [@dean2013]. A retry with exponential backoff and jitter recovers from transient faults, but only an idempotent operation may be retried safely, which is why idempotency keys exist.\n\n## Circuit breakers {#sec:breakers}\n\nA circuit breaker stops calling a dependency that keeps failing. ${edition > 1 ? "It is opened after five consecutive failures and probed after a cool-down." : "It was opened by the system after a number of failures have been observed by it and probed later."}\n\n${edition > 1 ? "## Hedged requests {#sec:hedging}\n\nA hedged request sends a second copy after the 95th percentile latency has passed, and uses whichever answer arrives first [@dean2013]. See [@sec:lamport] for ordering the replies.\n\n" : ""}${edition > 1 ? "[@brewer2000] discusses the trade-offs between consistency and availability." : "Trade-offs between consistency and availability are discussed by [@brewer2000]."} Read more at [the docs](https://example.com/docs) and [the old wiki](htp://wiki.example).\n`,
    };
    for (const [name, text] of Object.entries(ch)) writeFile(dir.join("chapters", name), text);
}

// =============================================================================
// Editorial checks
// =============================================================================
const DICTIONARY = new Set(
    (
        "a about above add adds after again against all also an and answer answers any are arrives as at available availability be before believed belong between book breaker breakers " +
        "build built by call calls capture carries chapter chapters circuit clock clocks code commonly concurrency consecutive consistency cool copy counter dependency decides discussed docs down drift each " +
        "edition end ends events event every example examples exercises explanation explicitly exponential failing failure failures faults finish first five follows for from handling hedged hope hoped how " +
        "idea idempotency idempotent in increment introduction is it its jitter keeps key keys latency literate logical many maximum message more not number observed of on online only opened operation order " +
        "ordering orders parallel passed past percentile physical probed programming read receipt receiver recovers replies request requests require requires response retried retries retry rule run " +
        "safely scale second section sends services short slowest solutions start stops system take takes the their this time timestamps to together trade-offs transient trusting tail used uses vector wall " +
        "we whichever which why wiki with without written exist exists javascript lamport offs see synchronised later been does reliable all hedging has have also own that but may calling discusses old"
    ).split(" "),
);
const BANNED = new MultiMatcher(["it is commonly believed", "very unique", "in order to", "utilize"]);
const PASSIVE = /\b(was|were|been|being|is|are)\s+\w+ed\b\s+by\b/i;
const deletions = (w, depth = 2) => {
    // w with up to `depth` characters removed
    const out = new Set([w]);
    let frontier = [w];
    for (let d = 0; d < depth; d++) {
        const next = [];
        for (const x of frontier)
            for (let i = 0; i < x.length; i++) {
                const y = x.slice(0, i) + x.slice(i + 1);
                if (!out.has(y)) {
                    out.add(y);
                    next.push(y);
                }
            }
        frontier = next;
    }
    return out;
};
const symspell = new Multimap();
for (const word of DICTIONARY) for (const del of deletions(word)) symspell.put(del, word);
function suggest(word) {
    const candidates = new Set();
    for (const del of deletions(word)) for (const c of symspell.get(del)) candidates.add(c);
    return [...candidates]
        .map((d) => [d, DamerauLevenshtein(word, d, { max: 2 })])
        .filter(([, d]) => d <= 2)
        .sortWith([(a, b) => a[1] - b[1], (a, b) => (a[0] < b[0] ? -1 : 1)])[0];
}
const unknownWords = new Set();
const bruteSuggest = (word) =>
    [...DICTIONARY]
        .map((d) => [d, DamerauLevenshtein(word, d, { max: 2 })])
        .filter(([, d]) => d <= 2)
        .sortWith([(a, b) => a[1] - b[1], (a, b) => (a[0] < b[0] ? -1 : 1)])[0];
const syllables = (w) =>
    Math.max(
        1,
        (
            w
                .toLowerCase()
                .replace(/e$/, "")
                .match(/[aeiouy]+/g) || []
        ).length,
    );
function editorial(chapter, text) {
    const issues = [];
    const prose = text
        .replace(/^---[\s\S]*?---/, "")
        .replace(/\{#[^}]+\}|\[@[^\]]+\]|\[\^\d+\]|\(https?:[^)]*\)|https?:\S+|`[^`]*`/g, " ");
    prose.split("\n").forEach((line, i) => {
        for (const { 0: w, index } of line.matchAll(/[A-Za-z][a-z'-]+/g)) {
            if (line.startsWith("://", index + w.length)) continue;
            const lw = w.toLowerCase().replace(/'s$/, "");
            if (!DICTIONARY.has(lw) && !/^[A-Z]/.test(w) && lw.length > 2) {
                const sug = suggest(lw);
                unknownWords.add(lw);
                if (sug)
                    issues.push({ chapter, line: i + 1, kind: "spelling", text: `"${w}" — did you mean "${sug[0]}"?` });
            }
        }
        const lower = line.toLowerCase();
        if (BANNED.test(lower))
            issues.push({
                chapter,
                line: i + 1,
                kind: "style",
                text: `banned phrase: "${lower.slice(BANNED.firstIn(lower).at, BANNED.firstIn(lower).at + 24)}…"`,
            });
        if (PASSIVE.test(line))
            issues.push({ chapter, line: i + 1, kind: "style", text: `passive voice: "${PASSIVE.exec(line)[0]}"` });
        for (const s of line.split(/(?<=[.!?])\s+/))
            if (s.split(/\s+/).length > 35)
                issues.push({
                    chapter,
                    line: i + 1,
                    kind: "style",
                    text: `sentence of ${s.split(/\s+/).length} words`,
                });
        for (const m of line.matchAll(/\]\(([^)]+)\)|(https?:\/\/\S+|htp:\/\/\S+)/g)) {
            const url = (m[1] || m[2]).replace(/[.)]+$/, "");
            try {
                const u = new URL(url);
                if (!["http:", "https:"].includes(u.protocol)) throw new Error();
            } catch {
                issues.push({ chapter, line: i + 1, kind: "link", text: `malformed link ${url}` });
            }
        }
    });
    const words = prose.match(/[A-Za-z]+/g) || [],
        sentences = Math.max(1, (prose.match(/[.!?](\s|$)/g) || []).length);
    const flesch =
        206.835 -
        1.015 * (words.length / sentences) -
        84.6 * (words.reduce((s, w) => s + syllables(w), 0) / words.length);
    return { issues, flesch };
}

// =============================================================================
// Build
// =============================================================================
function build(srcDir, outDir) {
    const meta = YAML.Parse(readFile(srcDir.join("book.yaml"))),
        bib = YAML.Parse(readFile(srcDir.join("bibliography.yaml"))),
        indexTerms = YAML.Parse(readFile(srcDir.join("index-terms.yaml")));
    const files = readDir(srcDir.join("chapters"))
        .map((e) => e.name)
        .filter((n) => n.endsWith(".md"))
        .sort();
    const chapters = files.map((f, ci) => {
        const raw = readFile(srcDir.join("chapters", f));
        const fm = FrontMatter.split(raw);
        return { file: f, n: ci + 1, raw, meta: YAML.Parse(fm.data), body: fm.body };
    });
    const anchors = new Map(),
        errors = [];
    for (const c of chapters) {
        // number sections: 1, 1.1, 1.2 …
        let sub = 0;
        c.body.split("\n").forEach((line) => {
            const h = /^(#{1,2}) (.+?) \{#(sec:[\w-]+)\}\s*$/.exec(line);
            if (!h) return;
            const num = h[1].length === 1 ? `${c.n}` : `${c.n}.${++sub}`;
            if (anchors.has(h[3])) errors.push(`${c.file}: duplicate anchor ${h[3]}`);
            anchors.set(h[3], { num, title: h[2], file: `ch${c.n}.xhtml` });
        });
    }
    const citeOrder = [];
    for (const c of chapters) {
        let footnotes = [];
        const lines = c.body.split("\n").filter((l) => {
            const fn = /^\[\^(\d+)\]: (.+)$/.exec(l);
            if (fn) {
                footnotes.push([fn[1], fn[2]]);
                return false;
            }
            return true;
        });
        let md = lines
            .map((line, i) =>
                line
                    .replace(
                        /^(#{1,2}) (.+?) \{#(sec:[\w-]+)\}\s*$/,
                        (_, hs, t, id) => `${hs} ${anchors.get(id).num} ${t}`,
                    )
                    .replace(/\[@(sec:[\w-]+)\]/g, (_, id) => {
                        const a = anchors.get(id);
                        if (!a) {
                            errors.push(`${c.file}:${i + 1}: unresolved reference ${id}`);
                            return "??";
                        }
                        return `[Section ${a.num}](${a.file}#${id.replace(":", "-")})`;
                    })
                    .replace(/\[@(\w+)\]/g, (_, key) => {
                        if (!bib[key]) {
                            errors.push(`${c.file}:${i + 1}: unknown citation ${key}`);
                            return "[?]";
                        }
                        if (!citeOrder.includes(key)) citeOrder.push(key);
                        return `[${citeOrder.indexOf(key) + 1}](refs.xhtml#ref-${key})`;
                    })
                    .replace(
                        /\[\^(\d+)\]/g,
                        (_, n) => `<sup><a href="#fn-${c.n}-${n}" id="fnref-${c.n}-${n}">${n}</a></sup>`,
                    ),
            )
            .join("\n");
        if (footnotes.length)
            md += `\n\n---\n\n${footnotes.map(([n, t]) => `<p id="fn-${c.n}-${n}"><sup>${n}</sup> ${t}</p>`).join("\n")}`;
        c.html = MarkdownToHTML(md, { allowRawHTML: true }).replace(/<h([12])>(\d+(?:\.\d+)?) /g, (m, lvl, num) => {
            const id = [...anchors].find(([, a]) => a.num === num)?.[0];
            return `<h${lvl} id="${id ? id.replace(":", "-") : ""}">${num} `;
        });
        c.text = HTMLText(HTMLParse(c.html));
        c.editorial = editorial(c.file, c.raw);
    }
    const sectionText = new Map(); // section number -> its own text, up to the next heading
    for (const c of chapters) {
        const heads = [...anchors.values()]
            .filter((a) => a.num.split(".")[0] === String(c.n))
            .map((a) => ({ a, at: c.text.indexOf(`${a.num} ${a.title}`) }))
            .filter((h) => h.at >= 0)
            .sort((x, y) => x.at - y.at);
        heads.forEach((h, i) =>
            sectionText.set(
                h.a.num,
                c.text.slice(h.at, i + 1 < heads.length ? heads[i + 1].at : c.text.length).toLowerCase(),
            ),
        );
    }
    const index = indexTerms.map((term) => ({
        term,
        where: [...sectionText].filter(([, t]) => t.includes(term)).map(([num]) => num),
    }));
    // EPUB 3
    const id = uuid5(NAMESPACE_URL, `https://example.press/books/${meta.title}`);
    const page = (title, body) =>
        `<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="${meta.language}"><head><title>${title}</title><link rel="stylesheet" href="style.css"/></head><body>${body}</body></html>`;
    const toXhtml = (html) => html.replace(/<(br|hr|img[^>]*)>/g, "<$1/>");
    const refs = `<h1>References</h1><ol>${citeOrder.map((k) => `<li id="ref-${k}">${bib[k].author}. <em>${bib[k].title}</em>. ${bib[k].venue}, ${bib[k].year}.</li>`).join("")}</ol>`;
    const idx = `<h1>Index</h1><ul>${index
        .filter((t) => t.where.length)
        .map((t) => `<li>${t.term}: ${t.where.join(", ")}</li>`)
        .join("")}</ul>`;
    const nav = `<nav epub:type="toc" id="toc"><h1>Contents</h1><ol>${chapters.map((c) => `<li><a href="ch${c.n}.xhtml">${c.n}. ${c.meta.title}</a></li>`).join("")}<li><a href="refs.xhtml">References</a></li><li><a href="index.xhtml">Index</a></li></ol></nav>`;
    const opf = new XmlWriter({ indent: 1 });
    opf.open("package", { xmlns: "http://www.idpf.org/2007/opf", version: "3.0", "unique-identifier": "bookid" });
    opf.open("metadata", { "xmlns:dc": "http://purl.org/dc/elements/1.1/" });
    for (const [k, v, a] of [
        ["dc:identifier", `urn:uuid:${id}`, { id: "bookid" }],
        ["dc:title", meta.title],
        ["dc:creator", meta.author],
        ["dc:language", meta.language],
        ["dc:publisher", meta.publisher],
    ]) {
        opf.open(k, a);
        opf.text(v);
        opf.close();
    }
    opf.open("meta", { property: "dcterms:modified" });
    opf.text("2026-06-01T00:00:00Z");
    opf.close();
    opf.close();
    const items = [
        ["nav", "nav.xhtml", "application/xhtml+xml", { properties: "nav" }],
        ["css", "style.css", "text/css"],
        ...chapters.map((c) => [`ch${c.n}`, `ch${c.n}.xhtml`, "application/xhtml+xml"]),
        ["refs", "refs.xhtml", "application/xhtml+xml"],
        ["index", "index.xhtml", "application/xhtml+xml"],
    ];
    opf.open("manifest");
    for (const [iid, href, type, extra] of items) {
        opf.open("item", { id: iid, href, "media-type": type, ...(extra || {}) });
        opf.close();
    }
    opf.close();
    opf.open("spine");
    for (const [iid] of items.filter(([i]) => i !== "css" && i !== "nav")) {
        opf.open("itemref", { idref: iid });
        opf.close();
    }
    opf.close();
    opf.close();
    const files_ = [
        { name: "mimetype", data: new TextEncoder().encode("application/epub+zip"), method: "store" },
        {
            name: "META-INF/container.xml",
            data: new TextEncoder().encode(
                `<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>`,
            ),
        },
        {
            name: "OEBPS/content.opf",
            data: new TextEncoder().encode(`<?xml version="1.0" encoding="utf-8"?>\n${opf.finish()}`),
        },
        { name: "OEBPS/nav.xhtml", data: new TextEncoder().encode(page("Contents", nav)) },
        {
            name: "OEBPS/style.css",
            data: new TextEncoder().encode(
                "body{font-family:serif;line-height:1.5} h1,h2{font-family:sans-serif} sup a{text-decoration:none}",
            ),
        },
        ...chapters.map((c) => ({
            name: `OEBPS/ch${c.n}.xhtml`,
            data: new TextEncoder().encode(page(c.meta.title, toXhtml(c.html))),
        })),
        { name: "OEBPS/refs.xhtml", data: new TextEncoder().encode(page("References", refs)) },
        { name: "OEBPS/index.xhtml", data: new TextEncoder().encode(page("Index", idx)) },
    ];
    const epub = ZipPack(files_.map((f) => ({ ...f, mtime: 1780272000 })));
    makeDir(outDir, { recursive: true });
    writeFile(outDir.join("book.epub"), epub);
    const single = new Template(
        `<!doctype html><html lang="{{lang}}"><head><meta charset="utf-8"><title>{{title}}</title></head><body><h1>{{title}}</h1><p>{{author}} · edition {{edition}}</p>{{{nav}}}{{#chapters}}<section>{{{html}}}</section>{{/chapters}}{{{refs}}}{{{index}}}</body></html>`,
    ).render({
        lang: meta.language,
        title: meta.title,
        author: meta.author,
        edition: meta.edition,
        nav,
        chapters: chapters.map((c) => ({ html: c.html.replace(/ch\d\.xhtml#/g, "#").replace(/refs\.xhtml#/g, "#") })),
        refs,
        index: idx,
    });
    writeFile(outDir.join("book.html"), single);
    writeFile(outDir.join("book.txt"), chapters.map((c) => c.text).join("\n\n"));
    return { meta, chapters, anchors, errors, citeOrder, index, epub, single, id };
}
function validateEpub(bytes) {
    const problems = [],
        list = ZipList(bytes);
    const first = ZipReadAt(bytes, 0);
    if (
        list[0].name !== "mimetype" ||
        dec.decode(first.data) !== "application/epub+zip" ||
        (first.method && first.method !== "store" && first.method !== 0)
    )
        problems.push("mimetype must be the first, uncompressed entry");
    const container = dec.decode(ZipRead(bytes, "META-INF/container.xml"));
    const opfPath = /full-path="([^"]+)"/.exec(container)[1];
    const opf = XMLParse(dec.decode(ZipRead(bytes, opfPath)));
    const walk = (n, name, out = []) => {
        if (n.name === name) out.push(n);
        for (const c of n.children || []) if (typeof c === "object") walk(c, name, out);
        return out;
    };
    const manifest = walk(opf, "item"),
        names = new Set(list.map((e) => e.name));
    for (const it of manifest) {
        const path = `OEBPS/${it.attrs.href}`;
        if (!names.has(path)) {
            problems.push(`manifest item ${it.attrs.href} is missing`);
            continue;
        }
        if (it.attrs["media-type"] === "application/xhtml+xml")
            try {
                XMLParse(dec.decode(ZipRead(bytes, path)));
            } catch (e) {
                problems.push(`${path} is not well-formed XML: ${e.message}`);
            }
    }
    for (const ref of walk(opf, "itemref"))
        if (!manifest.some((m) => m.attrs.id === ref.attrs.idref))
            problems.push(`spine references unknown item ${ref.attrs.idref}`);
    if (!manifest.some((m) => m.attrs.properties === "nav")) problems.push("no navigation document");
    return { problems, entries: list.length };
}

// =============================================================================
// Two editions
// =============================================================================
const results = {};
const ed = [];
for (const e of [1, 2]) {
    const src = work.join(`src-${e}`);
    writeManuscript(src, e);
    ed.push(build(src, work.join(`out-${e}`)));
}
const [b1, b2] = ed,
    v2 = validateEpub(b2.epub);
const ch3 = b2.chapters[2];
results[`SymSpell suggestions equal a brute-force dictionary scan for all ${unknownWords.size} unknown words`] =
    unknownWords.size > 0 && [...unknownWords].every((w) => String(suggest(w)) === String(bruteSuggest(w)));
results[`EPUB validates (${v2.entries} entries${v2.problems.length ? ": " + v2.problems.join("; ") : ""})`] =
    v2.problems.length === 0;
results["cross-references resolve to numbered sections"] =
    b2.errors.length === 0 && b2.anchors.get("sec:hedging").num === "3.2" && /Section 2\.1/.test(ch3.html);
results["citations are numbered in order of first use"] =
    b2.citeOrder.join() === "knuth1984,lamport1978,dean2013,brewer2000" &&
    ch3.html.includes('href="refs.xhtml#ref-dean2013">3</a>');
results["footnotes are numbered per chapter and linked both ways"] =
    b2.chapters[1].html.includes('id="fn-2-1"') && b2.chapters[0].html.includes('id="fnref-1-1"');
const issues = (b) => b.chapters.flatMap((c) => c.editorial.issues);
results["edition 1's passive sentence is flagged and edition 2 fixed it"] =
    issues(b1).some((i) => i.kind === "style" && /passive/.test(i.text)) &&
    !issues(b2).some((i) => /passive/.test(i.text));
results["a banned phrase and a malformed link are reported with their location"] =
    issues(b2).some((i) => /banned phrase/.test(i.text)) &&
    issues(b2).some((i) => i.kind === "link" && /htp:/.test(i.text));
results["index terms point at the sections that use them"] =
    b2.index.find((t) => t.term === "tail latency").where.includes("3") &&
    b2.index.find((t) => t.term === "logical clock").where.some((w) => w.startsWith("2"));
results["the dictionary accepts every word of edition 2"] = !issues(b2).some((i) => i.kind === "spelling");
results["the book identifier is stable across editions (UUIDv5)"] = b1.id === b2.id;
const broken = work.join("src-bad");
writeManuscript(broken, 2);
writeFile(
    broken.join("chapters/04-extra.md"),
    "---\nid: extra\ntitle: Extra\nstatus: draft\n---\n# Extra {#sec:extra}\n\nSee [@sec:nowhere] and [@smith2020].\n",
);
const bad = build(broken, work.join("out-bad"));
results["unresolved references and citations fail the build with file:line"] =
    bad.errors.some((e) => /04-extra\.md:\d+: unresolved reference sec:nowhere/.test(e)) &&
    bad.errors.some((e) => /unknown citation smith2020/.test(e));
const diff = b2.chapters.map((c, i) => {
    const h = DiffWords(b1.chapters[i].text, c.text);
    return {
        file: c.file,
        added: h.filter((x) => x.op === 1).reduce((s, x) => s + x.text.split(/\s+/).filter(Boolean).length, 0),
        removed: h.filter((x) => x.op === -1).reduce((s, x) => s + x.text.split(/\s+/).filter(Boolean).length, 0),
    };
});
results["the edition diff shows chapter 2 unchanged and chapter 3 extended"] =
    diff[1].added === 0 && diff[1].removed === 0 && diff[2].added > 20;

print("publishing-pipeline");
print(
    `  book       "${b2.meta.title}" edition ${b2.meta.edition}: ${b2.chapters.length} chapters, ${b2.anchors.size} sections, ${b2.citeOrder.length} references, EPUB ${(b2.epub.length / 1024).toFixed(1)} KiB (${SHA256Hex(b2.epub).slice(0, 12)})`,
);
for (const c of b2.chapters)
    print(
        `  ${c.file.padEnd(20)} Flesch ${c.editorial.flesch.toFixed(0).padStart(3)}  issues: ${c.editorial.issues.map((i) => `${i.kind}@${i.line} ${i.text}`).join("; ") || "none"}`,
    );
print(
    `  index      ${b2.index
        .filter((t) => t.where.length)
        .map((t) => `${t.term} (${t.where.join(", ")})`)
        .join("; ")}`,
);
print(`  changes    ${diff.map((d) => `${d.file} +${d.added}/-${d.removed} words`).join(", ")}`);
print(`  broken     ${bad.errors.join("; ")}`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
