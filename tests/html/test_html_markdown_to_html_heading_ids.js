// covers: dyna:html.MarkdownToHTML
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_markdown_to_html_heading_ids");
import { MarkdownToHTML } from "dyna:html";

let pass = 0, fail = 0;
function eq(name, got, want) {
    if (got === want) pass++;
    else { fail++; print(`FAIL ${name}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); }
}

function ids(md) {
    const out = [];
    const re = /<h[1-6](?: id="([^"]*)")?>/g;
    let m;
    const html = MarkdownToHTML(md, { headingIds: true });
    while ((m = re.exec(html)) !== null) out.push(m[1] === undefined ? null : m[1]);
    return out;
}

function slugOracle(text) {
    let s = "";
    for (const ch of text) {
        const c = ch.codePointAt(0);
        if (c >= 0x80) s += ch;
        else if (/[0-9a-z_-]/.test(ch)) s += ch;
        else if (/[A-Z]/.test(ch)) s += ch.toLowerCase();
        else if (ch === " ") s += "-";
    }
    return s;
}
function dedupeOracle(slugs) {
    const occ = new Map();
    return slugs.map((orig) => {
        if (orig === "") return null;
        let r = orig;
        while (occ.has(r)) {
            occ.set(orig, occ.get(orig) + 1);
            r = orig + "-" + occ.get(orig);
        }
        occ.set(r, 0);
        return r;
    });
}

eq("off by default", MarkdownToHTML("# Hello"), "<h1>Hello</h1>\n");
eq("off explicit", MarkdownToHTML("# Hello", { headingIds: false }), "<h1>Hello</h1>\n");
eq("on", MarkdownToHTML("# Hello World", { headingIds: true }), '<h1 id="hello-world">Hello World</h1>\n');
eq("setext h1", MarkdownToHTML("Title Here\n===", { headingIds: true }), '<h1 id="title-here">Title Here</h1>\n');
eq("setext h2", MarkdownToHTML("Sub\n---", { headingIds: true }), '<h2 id="sub">Sub</h2>\n');

eq("github-slugger repeat chain", JSON.stringify(ids("# foo\n\n# foo\n\n# foo-1\n\n# foo")), JSON.stringify(["foo", "foo-1", "foo-1-1", "foo-2"]));
eq("emphasis dropped", JSON.stringify(ids("## Hello *big* world")), JSON.stringify(["hello-big-world"]));
eq("code span", JSON.stringify(ids("# Use `map()` here")), JSON.stringify(["use-map-here"]));
eq("entities dropped", JSON.stringify(ids("# A & B <c> \"q\"")), JSON.stringify(["a--b-c-q"]));
eq("punct only -> no id", JSON.stringify(ids("# !!!\n\n# ???")), JSON.stringify([null, null]));
eq("non-ASCII kept as written (ASCII-only lowercasing)", JSON.stringify(ids("# Café Ünïcode")), JSON.stringify(["café-Ünïcode"]));
eq("underscore hyphen", JSON.stringify(ids("# snake_case and-kebab")), JSON.stringify(["snake_case-and-kebab"]));
eq("trailing hashes", JSON.stringify(ids("### Closed ###")), JSON.stringify(["closed"]));
eq("link text", JSON.stringify(ids("# See [the docs](https://x.y/z)")), JSON.stringify(["see-the-docs"]));
eq("raw html stripped", JSON.stringify(ids("# a <b>bold</b> z")), JSON.stringify(["a-bboldb-z"]));
__kitS.section("# a <b>bold</b> z", () => {
    const html = MarkdownToHTML("# a <b>bold</b> z", { headingIds: true, allowRawHTML: true });
    eq("raw html allowed", html, '<h1 id="a-bold-z">a <b>bold</b> z</h1>\n');
});
eq("id inside list item untouched", MarkdownToHTML("plain para", { headingIds: true }), "<p>plain para</p>\n");

let seed = 0x9e3779b9;
function rnd(n) {
    seed ^= seed << 13; seed >>>= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5; seed >>>= 0;
    return seed % n;
}
const alphabet = [];
for (let c = 0x21; c < 0x7f; c++) {
    const ch = String.fromCharCode(c);
    if ("\\`*_[]!<>&#\"~=-|'(){}+.:;".includes(ch)) continue;
    alphabet.push(ch);
}
alphabet.push("é", "ß", "中", " ", "-");
for (let t = 0; t < 400; t++) {
    const count = 1 + rnd(12);
    const texts = [];
    let md = "";
    for (let k = 0; k < count; k++) {
        let txt = alphabet[rnd(alphabet.length)];
        if (txt === " ") txt = "x";
        const len = rnd(10);
        for (let j = 0; j < len; j++) txt += alphabet[rnd(alphabet.length)];
        if (rnd(3) === 0 && texts.length) txt = texts[rnd(texts.length)];
        txt = txt.replace(/ +$/, "");
        if (txt.endsWith("-")) txt += "z";
        texts.push(txt);
        md += "#".repeat(1 + rnd(6)) + " " + txt + "\n\n";
    }
    const want = dedupeOracle(texts.map((x) => slugOracle(x.replace(/&/g, ""))));
    eq(`random ${t}`, JSON.stringify(ids(md)), JSON.stringify(want));
}

__kitS.section("# same", () => {
    let md = "";
    for (let i = 0; i < 3000; i++) md += "# same\n\n";
    const got = ids(md);
    eq("3000 repeats count", got.length, 3000);
    eq("3000 repeats last", got[2999], "same-2999");
    eq("3000 unique", new Set(got).size, 3000);
});
__kitS.end();

print(`markdown heading ids: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("heading ids failed");
