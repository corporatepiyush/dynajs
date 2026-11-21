/*
 * test_htmlkit_core.c -- src/core/htmlkit + src/core/markdown as standalone C
 * libraries (no engine; the SIMD dispatch table is linked and initialised).
 *
 * External oracles:
 *  - CommonMark 0.31.2 spec examples: ATX heading, setext heading, emphasis,
 *    strong emphasis, code span, inline link, fenced code escaping, tight
 *    bullet list, backslash escape (outputs copied from the spec).
 *  - WHATWG HTML named/numeric character references: &amp; &lt; &copy;
 *    &#65; &#x41; decode to & < U+00A9 A A; an unknown name stays literal.
 *  - WHATWG HTML serialization: text escapes & < >, attribute mode also ".
 * Properties: a 300-section document renders with raw HTML escaped when
 * allowRawHTML is off; selector compile accepts valid and refuses dangling
 * combinators; the sanitizer refuses javascript: in any letter case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-htmlkit.h"
#include "dyn-markdown.h"

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static int has(const uint8_t* p, size_t n, const char* needle)
{
    size_t k = strlen(needle), i;
    for (i = 0; p && i + k <= n; i++)
        if (!memcmp(p + i, needle, k))
            return 1;
    return 0;
}

static void md(const char* in, const char* want)
{
    hb_t out;
    int rc = dyn_md_render_html(in, strlen(in), 0, 0, &out);
    CHECK(rc == 0 && out.n == strlen(want) && !memcmp(out.p, want, out.n),
        "CommonMark %s -> %.*s, want %s", in, (int)out.n, out.p ? (const char*)out.p : "", want);
    hb_free(&out);
}

static void test_markdown(void)
{
    md("# foo", "<h1>foo</h1>\n");
    md("Foo *bar*\n=========", "<h1>Foo <em>bar</em></h1>\n");
    md("*foo bar*", "<p><em>foo bar</em></p>\n");
    md("**foo bar**", "<p><strong>foo bar</strong></p>\n");
    md("`foo`", "<p><code>foo</code></p>\n");
    md("[link](/uri)", "<p><a href=\"/uri\">link</a></p>\n");
    md("```\n<\n >\n```", "<pre><code>&lt;\n &gt;\n</code></pre>\n");
    md("- a\n- b", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
    md("\\*not emphasized*", "<p>*not emphasized*</p>\n");
    {
        hb_t big, out;
        int i, rc;
        hb_init(&big);
        for (i = 0; i < 300; i++) {
            char line[160];
            snprintf(line, sizeof line, "## H %d\n\nPara **b** <script>x</script> [l](https://x.org/%d)\n\n", i, i);
            hb_puts(&big, line);
        }
        rc = dyn_md_render_html((const char*)big.p, big.n, 0, 1, &out);
        CHECK(rc == 0 && out.n > big.n, "large document renders (%zu bytes)", out.n);
        CHECK(rc == 0 && !has(out.p, out.n, "<script>"), "raw <script> is escaped with allowRawHTML off");
        CHECK(rc == 0 && has(out.p, out.n, "id=\"h-299\""), "headingIds slugs every heading");
        hb_free(&out);
        hb_free(&big);
    }
}

static void ent(const char* in, const char* want)
{
    hb_t out;
    hb_init(&out);
    ht_decode(in, strlen(in), &out);
    CHECK(out.n == strlen(want) && !memcmp(out.p, want, out.n),
        "decode %s -> %.*s, want %s", in, (int)out.n, out.p ? (const char*)out.p : "", want);
    hb_free(&out);
}

static void test_entities(void)
{
    ent("a &amp; b", "a & b");
    ent("&lt;x&gt;", "<x>");
    ent("&copy;", "\xc2\xa9");
    ent("&#65;&#x41;", "AA");
    ent("&notanentity;", "&notanentity;");
}

static void esc(const char* in, int attr, const char* want)
{
    hb_t out;
    hb_init(&out);
    ht_escape(&out, in, strlen(in), attr);
    CHECK(out.n == strlen(want) && !memcmp(out.p, want, out.n),
        "escape(%s, attr=%d) -> %.*s, want %s", in, attr, (int)out.n, out.p ? (const char*)out.p : "", want);
    hb_free(&out);
}

static void test_escape(void)
{
    char longs[300];
    esc("<a href=\"x\">&</a>", 0, "&lt;a href=\"x\"&gt;&amp;&lt;/a&gt;");
    esc("say \"hi\" & bye", 1, "say &quot;hi&quot; &amp; bye");
    memset(longs, 'x', sizeof longs - 2);
    longs[sizeof longs - 2] = '<';
    longs[sizeof longs - 1] = 0;
    {
        hb_t out;
        hb_init(&out);
        ht_escape(&out, longs, strlen(longs), 0);
        CHECK(out.n == strlen(longs) + 3 && !memcmp(out.p + out.n - 4, "&lt;", 4),
            "escape finds a < past the SIMD probe window");
        hb_free(&out);
    }
}

static void test_selector_sanitizer(void)
{
    char err[128];
    sel_t* s = sel_compile("div.a#x > p, a[href^=https]", 27, err, sizeof err);
    CHECK(s != NULL, "selector compiles: %s", err);
    if (s)
        sel_free(s);
    err[0] = 0;
    s = sel_compile("div >", 5, err, sizeof err);
    CHECK(s == NULL && err[0], "a dangling combinator is refused (%s)", err);
    if (s)
        sel_free(s);
    {
        san_t sz;
        memset(&sz, 0, sizeof sz);
        CHECK(!san_proto_ok(&sz, "a", 1, "href", 4, "JaVaScRiPt:alert(1)", 19), "javascript: refused in any case");
        CHECK(san_proto_ok(&sz, "a", 1, "href", 4, "https://e.com/", 14), "https allowed by default");
        CHECK(ht_is_srcset_attr("srcset", 6) && ht_is_single_url_attr("href", 4), "URL attribute tables");
    }
}

int main(void)
{
    simd_init();
    test_markdown();
    test_entities();
    test_escape();
    test_selector_sanitizer();
    printf("test_htmlkit_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
