// CMS publisher — markdown content to a fingerprinted static site with feeds, search, link checks, a crawl and a signed deploy.
//
// WHAT IT IS
//   A publishing pipeline for a docs/blog site, from content files to a deployable, verified artifact:
//
//   content       markdown posts with YAML front matter (title, date, author, tags, draft); authors in a YAML
//                 file; every post's slug, date and author validated, with errors reported per file
//   rendering     markdown to HTML, a layout template, tag pages, a paginated index
//   assets        CSS/JS fingerprinted by content hash (app.css -> app.3f2a9c1e.css) and every reference
//                 in every page rewritten to the fingerprinted name, so they can be cached forever
//   feeds         RSS 2.0 written with a streaming XML writer; sitemap.xml
//   search        a JSON index of terms -> pages for client-side search
//   incremental   a build cache keyed by content hash: an unchanged post is not re-rendered
//   verification  internal links checked against the output; the built site is served and crawled
//                 (robots, extraction) to prove every page is reachable and titled
//   delivery      every file precompressed (gzip) and listed in a deploy manifest of SHA-256 digests,
//                 signed with Ed25519 so the CDN uploader can refuse a modified build
//   related       each post's tags are a RoaringBitmap of interned tag ids; "related posts" is the native Jaccard
//                 between bitmaps, written to /related.json; the build also keeps a BM25 InvertedIndex of the posts
//                 so the self-test can query the site the way its search box would
//
// MODULES  dyna:config (FrontMatter) · dyna:yaml · dyna:html · dyna:xml · dyna:time · dyna:file · dyna:hash
//          dyna:compress · dyna:crypto · dyna:validate · dyna:http (HTTPServer, HTTPClient) · dyna:scrape
//          · dyna:structures (Interner, RoaringBitmap, Multimap, InvertedIndex)
//
// RUN      dynajs examples/web/cms-publisher.js --content ./site --out ./public     build a real site
//          dynajs examples/web/cms-publisher.js                                     builds a sample site twice
//                                                                                   (the second time incrementally)

import { FrontMatter } from "dyna:config";
import { Parse as parseYAML } from "dyna:yaml";
import { MarkdownToHTML, Template, HTMLParse, HTMLStringify, Selector, HTMLText, rewriteLinks } from "dyna:html";
import { XmlWriter, XMLParse } from "dyna:xml";
import { parseDate, Format } from "dyna:time";
import { Path, makeTempDir, makeDir, readFile, writeFile, glob, removeAll, exists, readBytes } from "dyna:file";
import { SHA256Hex, XXH3_64 } from "dyna:hash";
import { gzip, gunzip } from "dyna:compress";
import * as crypto from "dyna:crypto";
import { IsSlug } from "dyna:validate";
import { Interner, RoaringBitmap, Multimap, InvertedIndex } from "dyna:structures";
import { HTTPServer, HTTPClient } from "dyna:http";
import { Fetcher, Crawl, Extractor } from "dyna:scrape";
import { Command } from "dyna:cli";

const { options } = new Command("cms-publisher")
    .option("--content <dir>", "content directory", { type: "string" })
    .option("--out <dir>", "output directory", { type: "string" })
    .option("--site <url>", "public base URL", { type: "string", default: "https://docs.example.test" })
    .parse(scriptArgs.slice(1));
const selfTest = !options.content;
const work = selfTest ? makeTempDir("cms") : null;
const contentDir = selfTest ? work.join("content") : new Path(options.content);
const outDir = selfTest ? work.join("public") : new Path(options.out || "public");
const SITE = options.site;
if (selfTest) writeSampleContent(contentDir);

// =============================================================================
// Build
// =============================================================================
const layout = new Template(`<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>{{title}} · Example Docs</title>
<link rel="stylesheet" href="/assets/site.css"><script src="/assets/site.js" defer></script></head>
<body><header><a href="/index.html">Example Docs</a> · <a href="/tags/index.html">Tags</a></header>
<main><h1>{{title}}</h1>{{#meta}}<p class="meta">{{date}} · {{author}} · {{#tags}}<a href="/tags/{{.}}.html">#{{.}}</a> {{/tags}}</p>{{/meta}}
{{{body}}}</main></body></html>`);
const listItem = new Template(`<li><a href="{{url}}">{{title}}</a> <small>{{date}}</small></li>`);
const longDate = new Format("Jan 2, 2006");

function build(cache) {
    const errors = [],
        posts = [];
    const authors = parseYAML(readFile(contentDir.join("authors.yaml")));
    let rendered = 0,
        reused = 0;
    for (const rel of glob("posts/*.md", { cwd: contentDir })) {
        const src = readFile(contentDir.join(rel));
        const { data, body } = FrontMatter.split(src);
        const fm = data ? parseYAML(data) : {};
        const slug = rel.basenameWithout(".md");
        const problems = [];
        if (!IsSlug(slug)) problems.push(`slug "${slug}" is not lowercase-hyphenated`);
        if (!fm.title) problems.push("missing title");
        let date = null;
        try {
            date = parseDate(String(fm.date));
        } catch {
            problems.push(`bad date "${fm.date}"`);
        }
        if (!authors[fm.author]) problems.push(`unknown author "${fm.author}"`);
        if (problems.length) {
            errors.push({ file: rel.toString(), problems });
            continue;
        }
        if (fm.draft) continue;
        const key = XXH3_64(src);
        let html = cache.get(slug)?.key === key ? cache.get(slug).html : null;
        if (html) reused++;
        else {
            html = MarkdownToHTML(body);
            cache.set(slug, { key, html });
            rendered++;
        }
        posts.push({
            slug,
            url: `/posts/${slug}.html`,
            title: fm.title,
            date,
            author: authors[fm.author].name,
            tags: fm.tags || [],
            html,
            text: HTMLText(HTMLParse(html)),
        });
    }
    posts.sort((a, b) => b.date.compare(a.date));

    const files = new Map(); // output path -> content (string)
    const page = (title, body, meta) => layout.render({ title, body, meta });
    for (const p of posts)
        files.set(
            p.url,
            page(p.title, p.html, { date: longDate.format(p.date.epochDay * 86400), author: p.author, tags: p.tags }),
        );
    const pages = Math.ceil(posts.length / 5);
    for (let i = 0; i < pages; i++) {
        const items = posts
            .slice(i * 5, i * 5 + 5)
            .map((p) => listItem.render({ ...p, date: p.date.toString() }))
            .join("");
        const nav =
            (i > 0 ? `<a href="/${i === 1 ? "index" : "page-" + i}.html">Newer</a> ` : "") +
            (i + 1 < pages ? `<a href="/page-${i + 2}.html">Older</a>` : "");
        files.set(
            i === 0 ? "/index.html" : `/page-${i + 1}.html`,
            page(i === 0 ? "Latest" : `Page ${i + 1}`, `<ul>${items}</ul><nav>${nav}</nav>`),
        );
    }
    const byTag = new Multimap();
    for (const p of posts) for (const t of p.tags) byTag.put(t, p);
    const tags = new Map(byTag.keys().map((t) => [t, byTag.get(t)]));
    for (const [t, ps] of tags)
        files.set(
            `/tags/${t}.html`,
            page(`#${t}`, `<ul>${ps.map((p) => listItem.render({ ...p, date: p.date.toString() })).join("")}</ul>`),
        );
    files.set(
        "/tags/index.html",
        page(
            "Tags",
            `<ul>${[...tags.keys()]
                .sort()
                .map((t) => `<li><a href="/tags/${t}.html">#${t}</a> (${tags.get(t).length})</li>`)
                .join("")}</ul>`,
        ),
    );

    // Fingerprint assets, then rewrite every reference in every page.
    const assetMap = new Map();
    for (const rel of glob("assets/*", { cwd: contentDir })) {
        const bytes = readBytes(contentDir.join(rel));
        const ext = rel.extname,
            name = rel.basenameWithout(ext);
        const hashed = `/assets/${name}.${SHA256Hex(bytes).slice(0, 8)}${ext}`;
        assetMap.set(`/assets/${rel.basename}`, hashed);
        files.set(hashed, new TextDecoder().decode(bytes));
    }
    for (const [path, html] of files) {
        if (!path.endsWith(".html")) continue;
        const doc = HTMLParse(html);
        rewriteLinks(doc, (url) => assetMap.get(url));
        files.set(
            path,
            "<!doctype html>\n" +
                doc
                    .filter((n) => typeof n !== "string")
                    .map(HTMLStringify)
                    .join(""),
        );
    }

    // RSS with a streaming writer, sitemap, search index.
    const rss = new XmlWriter({ indent: 1 });
    rss.open("rss", { version: "2.0" });
    rss.open("channel");
    for (const [k, v] of [
        ["title", "Example Docs"],
        ["link", SITE],
        ["description", "Release notes and guides"],
    ]) {
        rss.open(k);
        rss.text(v);
        rss.close();
    }
    for (const p of posts.slice(0, 20)) {
        rss.open("item");
        for (const [k, v] of [
            ["title", p.title],
            ["link", SITE + p.url],
            ["guid", SITE + p.url],
            ["pubDate", new Date(p.date.epochDay * 86400000).toUTCString()],
            ["description", p.text.slice(0, 200)],
        ]) {
            rss.open(k);
            rss.text(v);
            rss.close();
        }
        rss.close();
    }
    files.set("/feed.xml", '<?xml version="1.0" encoding="UTF-8"?>\n' + rss.finish());
    const sm = new XmlWriter();
    sm.open("urlset", { xmlns: "http://www.sitemaps.org/schemas/sitemap/0.9" });
    for (const path of [...files.keys()].filter((p) => p.endsWith(".html")).sort()) {
        sm.open("url");
        sm.open("loc");
        sm.text(SITE + path);
        sm.close();
        sm.close();
    }
    files.set("/sitemap.xml", '<?xml version="1.0" encoding="UTF-8"?>\n' + sm.finish());
    const index = {};
    for (const p of posts)
        for (const term of new Set(p.text.toLowerCase().match(/[a-z]{4,}/g) || [])) (index[term] ||= []).push(p.slug);
    files.set("/search-index.json", JSON.stringify(index));
    const tagIds = new Interner(),
        tagBits = posts.map((p) => new RoaringBitmap(tagIds.internMany(p.tags)));
    const related = Object.fromEntries(
        posts.map((p, i) => [
            p.slug,
            posts
                .map((q, j) => [q.slug, i === j ? -1 : tagBits[i].jaccard(tagBits[j])])
                .filter(([, sim]) => sim > 0)
                .sortWith([(a, b) => b[1] - a[1], (a, b) => (a[0] < b[0] ? -1 : 1)])
                .take(3)
                .map(([slug, sim]) => ({ slug, sim: sim.round(3) })),
        ]),
    );
    files.set("/related.json", JSON.stringify(related));
    const search = new InvertedIndex();
    posts.forEach((p, i) => search.add(i, `${p.title} ${p.title} ${p.text}`));
    files.set("/robots.txt", `User-agent: *\nAllow: /\nSitemap: ${SITE}/sitemap.xml\n`);
    return { files, posts, errors, rendered, reused, tags, related, tagBits, search };
}

function checkLinks(files) {
    const broken = [];
    const anchors = new Selector("a[href], link[href], script[src]");
    for (const [path, html] of files) {
        if (!path.endsWith(".html")) continue;
        for (const el of anchors.all(HTMLParse(html))) {
            const href = el.attrs.href || el.attrs.src;
            if (href.startsWith("/") && !files.has(href.split("#")[0])) broken.push({ page: path, href });
        }
    }
    return broken;
}

function write(files) {
    removeAll(outDir);
    const manifest = [];
    for (const [path, content] of files) {
        const dest = outDir.join("." + path);
        makeDir(dest.dirname, { recursive: true });
        writeFile(dest, content);
        const gz = gzip(content, 9);
        writeFile(new Path(dest + ".gz"), gz);
        manifest.push({
            path,
            sha256: SHA256Hex(content),
            bytes: new TextEncoder().encode(content).length,
            gzipBytes: gz.length,
        });
    }
    manifest.sort((a, b) => a.path.localeCompare(b.path));
    const body = JSON.stringify({ site: SITE, files: manifest });
    let signature = null,
        publicKey = null;
    if (typeof crypto.Ed25519Generate === "function") {
        const keys = crypto.Ed25519Generate(); // in production: the release key from a vault
        signature = crypto.Ed25519Sign(keys.privateKey, body);
        publicKey = keys.publicKey;
    }
    writeFile(outDir.join("deploy-manifest.json"), body);
    return { manifest, body, signature, publicKey };
}

// =============================================================================
// Run
// =============================================================================
const cache = new Map();
const first = build(cache);
const broken = checkLinks(first.files);
const out = write(first.files);
print(
    `cms-publisher: ${first.posts.length} posts, ${first.files.size} files, ${first.errors.length} rejected, ${broken.length} broken links`,
);
for (const e of first.errors) print(`  ✗ ${e.file}: ${e.problems.join("; ")}`);

if (selfTest) {
    const results = {};
    results[`11 published, 2 rejected (${first.posts.length}, ${first.errors.length})`] =
        first.posts.length === 11 && first.errors.length === 2;
    results["validation names each problem"] =
        first.errors.some((e) => e.problems.some((p) => /unknown author/.test(p))) &&
        first.errors.some((e) => e.problems.some((p) => /bad date/.test(p)));
    results[`no broken internal links (${JSON.stringify(broken.slice(0, 3))})`] = broken.length === 0;
    const indexHtml = first.files.get("/index.html");
    results["asset references are fingerprinted"] =
        /\/assets\/site\.[0-9a-f]{8}\.css/.test(indexHtml) && !indexHtml.includes('"/assets/site.css"');
    const someSlug = first.posts[0].slug,
        top = first.related[someSlug][0];
    results[`related posts share tags (${someSlug} -> ${top?.slug} at Jaccard ${top?.sim})`] = Object.entries(
        first.related,
    ).every(([slug, rel]) =>
        rel.every((r) => {
            const a = first.posts.find((p) => p.slug === slug).tags,
                b = first.posts.find((p) => p.slug === r.slug).tags;
            const inter = a.filter((t) => b.includes(t)).length;
            return inter > 0 && Math.abs(inter / new Set([...a, ...b]).size - r.sim) < 1e-3;
        }),
    );
    const [hit] = first.search.search(first.posts[2].title, { mode: "and", limit: 1 });
    results["searching a post's title ranks that post first"] = hit && first.posts[hit[0]].slug === first.posts[2].slug;
    results["11 posts paginate into 3 pages of 5"] =
        first.files.has("/page-3.html") && !first.files.has("/page-4.html");
    const feed = XMLParse(first.files.get("/feed.xml"));
    const items = feed.children.find((c) => c.name === "channel").children.filter((c) => c.name === "item");
    results["RSS lists every post"] = items.length === 11;
    results["search index finds a post by a term in its body"] = JSON.parse(
        first.files.get("/search-index.json"),
    ).postgres?.includes("connection-pooling");
    results["precompressed files decompress to the originals"] =
        gunzip(readBytes(outDir.join("index.html.gz")), { asString: true }) === readFile(outDir.join("index.html"));
    if (out.signature) {
        results["the deploy manifest verifies"] = crypto.Ed25519Verify(
            out.publicKey,
            readFile(outDir.join("deploy-manifest.json")),
            out.signature,
        );
        results["an edited manifest does not verify"] = !crypto.Ed25519Verify(
            out.publicKey,
            out.body.replace('"bytes":', '"bytes":1'),
            out.signature,
        );
    }

    // Incremental rebuild: change one post, everything else comes from the cache.
    const changed = contentDir.join("posts/release-2-0.md");
    writeFile(changed, readFile(changed).replace("faster", "much faster"));
    const second = build(cache);
    results[`incremental build re-rendered ${second.rendered}, reused ${second.reused}`] =
        second.rendered === 1 && second.reused === 10;

    // Serve the built site and crawl it: every page reachable from the index, each with its title.
    const routes = {};
    for (const [path, content] of second.files)
        routes[path] = {
            body: content,
            contentType: path.endsWith(".html")
                ? "text/html"
                : path.endsWith(".xml")
                  ? "application/xml"
                  : "text/plain",
        };
    const server = new HTTPServer({ port: 0, routes });
    server.start();
    const client = new HTTPClient();
    const fetcher = new Fetcher({ agent: "site-check/1.0", client, minDelayMs: 0, allowPrivateHosts: true });
    const extractor = new Extractor(
        { title: { sel: new Selector("h1") }, links: { sel: new Selector("a"), attr: "href", all: true } },
        { text: HTMLText },
    );
    const crawl = new Crawl(fetcher, { maxPages: 100, maxDepth: 6, linkField: "links" }).start(
        `http://127.0.0.1:${server.port}/index.html`,
        extractor,
        HTMLParse,
    );
    const crawled = [...crawl];
    const htmlPages = [...second.files.keys()].filter((p) => p.endsWith(".html"));
    results[`crawl reached ${crawled.length}/${htmlPages.length} pages, all titled`] =
        crawled.length === htmlPages.length && crawled.every((p) => p.status === 200 && p.value.title);
    print(
        `  incremental rebuild: ${second.rendered} rendered, ${second.reused} from cache; crawl ${crawled.length} pages; manifest ${out.manifest.length} files${out.signature ? ", Ed25519-signed" : ""}`,
    );
    print(`  tags: ${[...second.tags.keys()].sort().join(", ")}`);
    crawl.close();
    fetcher.close();
    client.close();
    server.close();
    removeAll(work);
    check(results);
}

// ---- sample content: a small product's docs and release notes -----------------
function writeSampleContent(dir) {
    makeDir(dir.join("posts"), { recursive: true });
    makeDir(dir.join("assets"), { recursive: true });
    writeFile(dir.join("authors.yaml"), "mira:\n  name: Mira Okafor\njon:\n  name: Jon Park\n");
    writeFile(
        dir.join("assets/site.css"),
        "body { font: 16px/1.5 system-ui; max-width: 46rem; margin: auto }\n.meta { color: #666 }\n",
    );
    writeFile(dir.join("assets/site.js"), "document.documentElement.classList.add('js');\n");
    const posts = [
        [
            "release-2-0",
            "Release 2.0",
            "2026-03-02",
            "mira",
            ["release"],
            "Version 2.0 is faster and smaller. Startup is faster by 40%.",
        ],
        ["release-1-9", "Release 1.9", "2026-02-02", "mira", ["release"], "Bug fixes and a new export format."],
        [
            "connection-pooling",
            "Connection pooling with Postgres",
            "2026-01-20",
            "jon",
            ["guide", "postgres"],
            "Use a pool of Postgres connections; size it to your CPU count.",
        ],
        [
            "caching-guide",
            "Caching strategies",
            "2026-01-12",
            "jon",
            ["guide", "performance"],
            "Cache at the edge first. See [pooling](/posts/connection-pooling.html).",
        ],
        [
            "observability",
            "Observability basics",
            "2026-01-05",
            "mira",
            ["guide", "operations"],
            "Logs, metrics and traces, and when to reach for each.",
        ],
        ["release-1-8", "Release 1.8", "2025-12-15", "mira", ["release"], "Introduces structured logging."],
        [
            "security-checklist",
            "Security checklist",
            "2025-12-01",
            "jon",
            ["guide", "security"],
            "Rotate keys, pin dependencies, review access every quarter.",
        ],
        ["release-1-7", "Release 1.7", "2025-11-10", "mira", ["release"], "Smaller binaries."],
        [
            "migrations",
            "Database migrations",
            "2025-10-28",
            "jon",
            ["guide", "postgres"],
            "Every migration is reversible and checksummed.",
        ],
        ["release-1-6", "Release 1.6", "2025-10-01", "mira", ["release"], "Windows support preview."],
        ["roadmap-2026", "Roadmap 2026", "2025-09-15", "mira", ["planning"], "What we are building next year."],
    ];
    for (const [slug, title, date, author, tags, body] of posts)
        writeFile(
            dir.join(`posts/${slug}.md`),
            `---\ntitle: ${title}\ndate: ${date}\nauthor: ${author}\ntags: [${tags.join(", ")}]\n---\n# ${title}\n\n${body}\n\n- point one\n- point two\n`,
        );
    writeFile(
        dir.join("posts/guest-post.md"),
        "---\ntitle: Guest post\ndate: 2026-01-01\nauthor: someone\n---\nHello.\n",
    );
    writeFile(dir.join("posts/broken-date.md"), "---\ntitle: Broken\ndate: 2026-02-30\nauthor: jon\n---\nText.\n");
    writeFile(
        dir.join("posts/wip.md"),
        "---\ntitle: Work in progress\ndate: 2026-03-10\nauthor: jon\ndraft: true\n---\nNot yet.\n",
    );
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
