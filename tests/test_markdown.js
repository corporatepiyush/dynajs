/* test_markdown.js -- MarkdownToHTML in dyna:html (design 07).
 *
 * A markdown renderer's security property is that its OUTPUT is safe: raw HTML
 * in the source is escaped unless the caller opts in, and a link's scheme is
 * checked, because `[x](javascript:...)` is the same hole as a raw <a>.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_markdown.js
 */
import { MarkdownToHTML as MD } from "dyna:html";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + "\n    got  " + JSON.stringify(a) + "\n    want " + JSON.stringify(b));
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

/* ------------------------------------------------------------------ blocks */

eq(MD("# One\n"), "<h1>One</h1>\n", "an ATX heading");
eq(MD("###### Six\n"), "<h6>Six</h6>\n", "six levels");
eq(MD("####### Seven\n"), "<p>####### Seven</p>\n", "seven is a paragraph");
eq(MD("#NoSpace\n"), "<p>#NoSpace</p>\n", "and a hash needs its space");
eq(MD("## Two ##\n"), "<h2>Two</h2>\n", "a closing run is dropped");
eq(MD("Title\n=====\n"), "<h1>Title</h1>\n", "a setext h1");
eq(MD("Title\n-----\n"), "<h2>Title</h2>\n", "and h2");
eq(MD("text\n"), "<p>text</p>\n", "a paragraph");
eq(MD("a\nb\n"), "<p>a\nb</p>\n", "two lines are one paragraph");
eq(MD("a\n\nb\n"), "<p>a</p>\n<p>b</p>\n", "a blank line separates them");
eq(MD("---\n"), "<hr>\n", "a thematic break");
eq(MD("***\n"), "<hr>\n", "in stars");
eq(MD("- a\n- b\n"), "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n", "a bullet list");
eq(MD("1. a\n2. b\n"), "<ol>\n<li>a</li>\n<li>b</li>\n</ol>\n", "an ordered list");
eq(MD("> q\n"), "<blockquote>\n<p>q</p>\n</blockquote>\n", "a block quote");
eq(MD("> a\n> b\n"), "<blockquote>\n<p>a\nb</p>\n</blockquote>\n", "over two lines");
eq(MD("> # h\n"), "<blockquote>\n<h1>h</h1>\n</blockquote>\n",
   "whose content is parsed as blocks");
eq(MD("```\nx\n```\n"), "<pre><code>x\n</code></pre>\n", "a fenced code block");
eq(MD("```js\nx\n```\n"), '<pre><code class="language-js">x\n</code></pre>\n',
   "with an info string");
eq(MD("~~~\nx\n~~~\n"), "<pre><code>x\n</code></pre>\n", "a tilde fence");
eq(MD("    code\n"), "<pre><code>code\n</code></pre>\n", "an indented code block");
eq(MD("```\n# not a heading\n```\n"),
   "<pre><code># not a heading\n</code></pre>\n", "code is not markdown");
eq(MD(""), "", "empty input");
eq(MD("\n\n\n"), "", "blank lines alone");

/* -------------------------------------------------------------- inline */

eq(MD("*em*\n"), "<p><em>em</em></p>\n", "emphasis");
eq(MD("_em_\n"), "<p><em>em</em></p>\n", "with underscores");
eq(MD("**strong**\n"), "<p><strong>strong</strong></p>\n", "strong");
eq(MD("~~del~~\n"), "<p><del>del</del></p>\n", "strikethrough");
eq(MD("`code`\n"), "<p><code>code</code></p>\n", "a code span");
eq(MD("``a ` b``\n"), "<p><code>a ` b</code></p>\n", "a doubled fence holds one");
eq(MD("`<b>`\n"), "<p><code>&lt;b&gt;</code></p>\n", "code content is escaped");
eq(MD("a * b\n"), "<p>a * b</p>\n", "a lone star is text");
eq(MD("[t](u)\n"), '<p><a href="u">t</a></p>\n', "a link");
eq(MD('[t](u "T")\n'), '<p><a href="u" title="T">t</a></p>\n', "with a title");
eq(MD("[*t*](u)\n"), '<p><a href="u"><em>t</em></a></p>\n', "with markup inside");
eq(MD("![a](i.png)\n"), '<p><img src="i.png" alt="a"></p>\n', "an image");
eq(MD("[t]\n"), "<p>[t]</p>\n", "an incomplete link stays literal");
eq(MD("<https://e.com>\n"), '<p><a href="https://e.com">https://e.com</a></p>\n',
   "an autolink");
eq(MD("\\*not em\\*\n"), "<p>*not em*</p>\n", "a backslash escape");
eq(MD("a & b\n"), "<p>a &amp; b</p>\n", "an ampersand is escaped");

/* --------------------------------------------------------------- tables */

eq(MD("| a | b |\n|---|---|\n| 1 | 2 |\n"),
   "<table>\n<thead><tr><th>a</th><th>b</th></tr></thead>\n" +
   "<tbody><tr><td>1</td><td>2</td></tr></tbody>\n</table>\n", "a GFM table");
eq(MD("| a |\n| b |\n"), "<p>| a |\n| b |</p>\n",
   "without a delimiter row it is a paragraph");

/* ------------------------------------- THE SECURITY PROPERTY OF OUTPUT */

{
    const vectors = [
        ["<script>alert(1)</script>\n", "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n",
         "raw HTML is escaped, not passed through"],
        ["<img src=x onerror=alert(1)>\n",
         "<p>&lt;img src=x onerror=alert(1)&gt;</p>\n", "including an image"],
        ["[x](javascript:alert(1))\n", '<p><a href="">x</a></p>\n',
         "a javascript: link gets an empty href"],
        ["[x](JaVaScRiPt:alert(1))\n", '<p><a href="">x</a></p>\n', "in any casing"],
        ["[x](java\tscript:alert(1))\n", "<p>[x](java\tscript:alert(1))</p>\n",
         "with an embedded tab -- the destination ends at the tab, so this is "
         + "not a link at all"],
        ["[x](data:text/html,<script>)\n", '<p><a href="">x</a></p>\n', "a data: URL"],
        ["![x](javascript:alert(1))\n", '<p><img src="" alt="x"></p>\n',
         "and an image source"],
        ['[x](u" onmouseover="alert(1))\n',
         '<p>[x](u" onmouseover="alert(1))</p>\n',
         "a space ends the destination, so an injected attribute is not a link"],
        ['[x](u"onmouseover="alert)\n',
         '<p><a href="u&quot;onmouseover=&quot;alert">x</a></p>\n',
         "and a quote that DOES reach the href is escaped there"],
        ["<https://e.com\" onx=\"1>\n", "<p>&lt;https://e.com\" onx=\"1&gt;</p>\n",
         "and an autolink with a quote is not an autolink"],
    ];
    let bad = 0;
    for (const [input, want, what] of vectors) {
        const got = MD(input);
        if (got !== want) {
            bad++;
            print("  " + what + ": " + JSON.stringify(got) + "\n    want " +
                  JSON.stringify(want));
        }
    }
    assert(bad === 0, "every markdown payload renders safely (" +
                      (vectors.length - bad) + "/" + vectors.length + ")");
}
eq(MD("<b>x</b>\n", { allowRawHTML: true }), "<p><b>x</b></p>\n",
   "allowRawHTML is the explicit opt-in");
eq(MD("[ok](/rel/path)\n"), '<p><a href="/rel/path">ok</a></p>\n',
   "a relative URL has no scheme to refuse");
eq(MD("[ok](mailto:a@b.c)\n"), '<p><a href="mailto:a@b.c">ok</a></p>\n',
   "and mailto is allowed");

throws(() => MD(42), "the input must be a string");
throws(() => MD(), "and is required");
{
    /* No input may make the block loop stall: a renderer that stops making
     * progress hangs rather than fails, which is worse. */
    const odd = ["```", "> ", "- ", "|", "#", "    ", "***", "[", "![", "~~",
                 "\n\n```\n", "> > > x", "1.", "a\n=", "`"];
    let ok = 0;
    for (const s of odd) { MD(s); ok++; }
    assert(ok === odd.length, "every truncated construct terminates");
}

if (fails) {
    print("test_markdown: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_markdown failed");
}
print("test_markdown: " + n + " assertions, 0 failures");
