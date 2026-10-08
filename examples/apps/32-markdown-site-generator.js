// 32 · Markdown site generator — turns a folder of Markdown posts with front matter into a static site.
//
// WHAT IT SHOWS
//   - dyna:config FrontMatter + dyna:yaml: metadata at the top of each post
//   - dyna:html MarkdownToHTML, Template (escaping by default) and Sanitizer
//   - a tag index and a home page generated from the same data
//   - deterministic output: sorted inputs, so rebuilding an unchanged site changes no file
//
// RUN      dynajs examples/apps/32-markdown-site-generator.js [content-dir] [output-dir]

import { FrontMatter } from "dyna:config";
import { Parse as parseYaml } from "dyna:yaml";
import { MarkdownToHTML, Template, Sanitizer } from "dyna:html";
import { Path, glob, readFile, writeFile, makeDir, makeTempDir, removeAll, exists } from "dyna:file";
import { SHA256Hex } from "dyna:hash";

// Templates escape every {{value}} by default. {{{value}}} inserts raw HTML and
// is used only for content this program produced itself.
const pageTemplate = new Template(`<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>{{title}} — {{site}}</title></head>
<body><nav><a href="/index.html">{{site}}</a></nav>
<article><h1>{{title}}</h1><p class="meta">{{date}}{{#tags}} · <a href="/tags/{{.}}.html">{{.}}</a>{{/tags}}</p>
{{{content}}}
</article></body></html>
`);
const listTemplate = new Template(`<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>{{heading}} — {{site}}</title></head>
<body><h1>{{heading}}</h1><ul>
{{#posts}}<li><a href="/{{slug}}.html">{{title}}</a> <small>{{date}}</small></li>
{{/posts}}</ul></body></html>
`);

// Posts may come from contributors. Markdown allows inline HTML, so the
// rendered body is passed through an allow-list before it is published.
const sanitizer = new Sanitizer({
    allow: { p: [], h2: [], h3: [], ul: [], ol: [], li: [], em: [], strong: [], code: [], pre: [], blockquote: [], a: ["href"] },
    protocols: { "a.href": ["http", "https", "mailto"] },
});

const slugify = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");

function loadPost(path) {
    const { data, body } = FrontMatter.split(readFile(path));
    const meta = data ? parseYaml(data) : {};
    if (!meta.title) throw new Error(`${path.basename}: front matter needs a title`);
    return {
        title: String(meta.title),
        date: String(meta.date ?? ""),
        tags: (meta.tags ?? []).map(String).sort(),
        draft: meta.draft === true,
        slug: meta.slug ? slugify(String(meta.slug)) : slugify(path.basenameWithout(".md")),
        content: sanitizer.clean(MarkdownToHTML(body, { allowRawHTML: true })),
    };
}

function build(contentDir, outDir, site = "Field Notes") {
    const posts = glob("*.md", { cwd: contentDir })
        .map((rel) => loadPost(contentDir.join(String(rel))))
        .filter((p) => !p.draft)
        .sort((a, b) => b.date.localeCompare(a.date) || a.slug.localeCompare(b.slug));

    const written = [];
    const emit = (rel, html) => {
        const dest = outDir.join(rel);
        makeDir(dest.dirname, { recursive: true });
        // Skip the write when nothing changed, so file times (and CDN caches) stay put.
        if (!exists(dest) || SHA256Hex(readFile(dest)) !== SHA256Hex(html)) { writeFile(dest, html); written.push(rel); }
    };

    for (const post of posts) emit(post.slug + ".html", pageTemplate.render({ ...post, site }));
    emit("index.html", listTemplate.render({ site, heading: site, posts }));

    const byTag = new Map();
    for (const post of posts) for (const tag of post.tags) byTag.set(tag, [...(byTag.get(tag) ?? []), post]);
    for (const [tag, tagged] of [...byTag].sort()) emit(`tags/${slugify(tag)}.html`, listTemplate.render({ site, heading: "Tagged: " + tag, posts: tagged }));
    return { posts: posts.length, tags: byTag.size, written };
}

// ---- command line / self-test ---------------------------------------------
const args = scriptArgs.slice(1);
if (args.length >= 2) {
    const result = build(new Path(args[0]), new Path(args[1]));
    console.log(`built ${result.posts} posts, ${result.tags} tag pages, ${result.written.length} files written`);
} else {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const work = makeTempDir("site");
    const content = work.join("content"), out = work.join("public");
    makeDir(content);
    writeFile(content.join("hello-world.md"),
        "---\ntitle: Hello, World\ndate: 2026-01-10\ntags: [intro, meta]\n---\nWelcome to the **first** post.\n\n- one\n- two\n");
    writeFile(content.join("second.md"),
        "---\ntitle: Tips & <Tricks>\ndate: 2026-02-01\ntags: [meta]\n---\nSee [the docs](https://example.com).\n\n" +
        '<script>alert("xss")</script>\n\n[bad](javascript:alert(1))\n');
    writeFile(content.join("wip.md"), "---\ntitle: Unfinished\ndraft: true\n---\nNot yet.\n");

    const first = build(content, out);
    check(first.posts === 2 && first.tags === 2, "two published posts, two tags; the draft is skipped");
    const index = readFile(out.join("index.html"));
    check(index.indexOf("Tips") < index.indexOf("Hello"), "the newest post is listed first");
    const post = readFile(out.join("second.html"));
    check(post.includes("Tips &amp; &lt;Tricks&gt;"), "titles are escaped by the template");
    check(!post.includes("<script") && !post.includes("javascript:"), "script tags and javascript: links are stripped");
    check(post.includes('href="https://example.com"'), "ordinary links survive");
    check(readFile(out.join("hello-world.html")).includes("<strong>first</strong>"), "markdown is rendered");
    check(readFile(out.join("tags", "meta.html")).split("<li>").length === 3, "the tag page lists both posts");

    const second = build(content, out);
    check(second.written.length === 0, "rebuilding an unchanged site writes nothing");
    console.log(`self-test passed: ${first.written.length} files generated, 0 rewritten on rebuild`);
    removeAll(work);
}
