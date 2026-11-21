/*
 * test_xmlscan_core.c -- src/core/xmlscan (the dyna:xml pull scanner and
 * escaper) as a standalone C library: no engine; the SIMD dispatch table is
 * linked and initialised.
 *
 * Expected token streams come from the XML 1.0 (Fifth Edition) recommendation:
 *  - 2.5 comment example "<!-- declarations for <head> & <body> -->" is
 *    well-formed; "<!-- B+, B, or B--->" is not.
 *  - 2.6 processing instructions: target, then data after white space.
 *  - 2.7 CDATA example "<![CDATA[<greeting>Hello, world!</greeting>]]>".
 *  - 2.4 "]]>" may not appear literally in content.
 *  - 3.1 empty-element example <IMG align="left" src="http://www.w3.org/Icons/WWW/w3c_home" />,
 *    WFC Unique Att Spec (a repeated attribute is an error), WFC No < in
 *    Attribute Values.
 *  - 3.3.3 attribute-value normalization: tab and line feed become a space.
 *  - 4.1 character references &#60; and &#x3C; are '<'; WFC Legal Character
 *    refuses &#0; and a surrogate; 4.6 the five predefined entities; WFC
 *    Entity Declared refuses &foo; (no DTD).
 *  - 2.3 Names: a digit cannot start a Name; "_a.b-c:d" is one Name.
 *  - 4.3.3 a UTF-8 byte order mark is not part of the document.
 *  - 2.4 / 3.1: in content only < and & must be escaped (> is, for "]]>");
 *    in a quoted attribute value the quote must be, and tab/LF/CR survive
 *    3.3.3 normalization only as character references.
 * Property: the token stream is identical whether the document arrives whole
 * or one byte at a time (the scanner's streaming contract); error messages are
 * compared without their byte offset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-xmlscan.h"

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

static xml_scan_t S;
static xml_ev_t E;

static size_t put(char* out, size_t o, size_t cap, const char* tag, const char* p, size_t n)
{
    int k;
    if (o >= cap)
        return o;
    k = snprintf(out + o, cap - o, "%s(%.*s)", tag, (int)n, p ? p : "");
    return k > 0 ? o + (size_t)k : o;
}

static void feed(const char* doc, size_t n, size_t* fed, size_t chunk)
{
    size_t k = chunk && n - *fed > chunk ? chunk : n - *fed;
    xb_write(&S.buf, doc + *fed, k);
    *fed += k;
    if (*fed == n)
        S.final = 1;
}

static void trace(const char* doc, size_t chunk, char* out, size_t cap)
{
    size_t n = strlen(doc), fed = 0, o = 0;
    memset(&S, 0, sizeof S);
    memset(&E, 0, offsetof(xml_ev_t, attr));
    xb_init(&S.buf);
    xb_init(&E.text);
    xb_init(&E.abuf);
    out[0] = 0;
    feed(doc, n, &fed, chunk);
    for (;;) {
        int r;
        xml_scan_bom(&S);
        if (!S.bom_done && fed < n) {
            feed(doc, n, &fed, chunk);
            continue;
        }
        r = xml_step(&S, &E);
        if (r == XS_NEED) {
            if (fed < n) {
                feed(doc, n, &fed, chunk);
                continue;
            }
            o = put(out, o, cap, "NEED", NULL, 0);
            break;
        }
        if (r == XS_END)
            break;
        if (r == XS_ERR) {
            const char* at = strstr(S.err, " at byte");
            o = put(out, o, cap, "ERR", S.err, at ? (size_t)(at - S.err) : strlen(S.err));
            break;
        }
        if (E.kind == XEV_OPEN) {
            uint32_t a;
            o = put(out, o, cap, E.selfclose ? "E" : "O", E.name, E.nlen);
            for (a = 0; a < E.nattr; a++) {
                o = put(out, o, cap, "@", E.attr[a].n, E.attr[a].nlen);
                o = put(out, o, cap, "=", (const char*)E.abuf.p + E.attr[a].voff, E.attr[a].vlen);
            }
        } else if (E.kind == XEV_CLOSE) {
            o = put(out, o, cap, "C", E.name, E.nlen);
        } else if (E.kind == XEV_TEXT) {
            o = put(out, o, cap, "T", (const char*)E.text.p, E.text.n);
        } else if (E.kind == XEV_CDATA) {
            o = put(out, o, cap, "D", (const char*)E.text.p, E.text.n);
        } else if (E.kind == XEV_COMMENT) {
            o = put(out, o, cap, "M", (const char*)E.text.p, E.text.n);
        } else if (E.kind == XEV_PI) {
            o = put(out, o, cap, "P", E.name, E.nlen);
            o = put(out, o, cap, "", E.val, E.vlen);
        } else {
            o = put(out, o, cap, "S", NULL, 0);
        }
        xml_compact(&S);
    }
    xb_free(&S.buf);
    xb_free(&E.text);
    xb_free(&E.abuf);
}

static void doc(const char* in, const char* want)
{
    static char whole[8192], bytes[8192];
    trace(in, 0, whole, sizeof whole);
    CHECK(!strcmp(whole, want), "XML 1.0: %s\n    got  %s\n    want %s", in, whole, want);
    trace(in, 1, bytes, sizeof bytes);
    CHECK(!strcmp(whole, bytes), "one byte at a time: %s\n    got  %s\n    whole %s", in, bytes, whole);
}

static void test_spec(void)
{
    doc("<a x=\"1\" y='2'>hi &lt;&gt;&amp;&apos;&quot; &#60;&#x3C;</a>", "O(a)@(x)=(1)@(y)=(2)T(hi <>&'\" <<)C(a)");
    doc("<r><![CDATA[<greeting>Hello, world!</greeting>]]></r>", "O(r)D(<greeting>Hello, world!</greeting>)C(r)");
    doc("<r><!-- declarations for <head> & <body> --></r>", "O(r)M( declarations for <head> & <body> )C(r)");
    doc("<r><!-- B+, B, or B---></r>", "O(r)ERR(-- in a comment)");
    doc("<r><?target data here?></r>", "O(r)P(target)(data here)C(r)");
    doc("<IMG align=\"left\" src=\"http://www.w3.org/Icons/WWW/w3c_home\" />",
        "E(IMG)@(align)=(left)@(src)=(http://www.w3.org/Icons/WWW/w3c_home)");
    doc("<1a/>", "ERR(expected an element name)");
    doc("<_a.b-c:d/>", "E(_a.b-c:d)");
    doc("<r>&#0;</r>", "O(r)ERR(character reference is NUL)");
    doc("<r>&#xD800;</r>", "O(r)ERR(character reference is a surrogate)");
    doc("<r a=\"x\ty\nz\"/>", "E(r)@(a)=(x y z)");
    doc("<r>]]></r>", "O(r)ERR(]]> in text must be escaped)");
    doc("<r a=\"<\"/>", "ERR(< in an attribute value)");
    doc("<r>&foo;</r>", "O(r)ERR(unknown entity (no DTD is processed, so none can exist))");
    doc("\xEF\xBB\xBF<r/>", "E(r)");
    doc("<r a=\"1\" a=\"2\"/>", "ERR(duplicate attribute name)");
    doc("<r>caf\xC3\xA9 &#x1F600;</r>", "O(r)T(caf\xC3\xA9 \xF0\x9F\x98\x80)C(r)");
}

static void test_long_text(void)
{
    static char in[3000], want[3100];
    memcpy(in, "<r>", 3);
    memset(in + 3, 'x', 2000);
    memcpy(in + 2003, "&amp;y</r>", 11);
    snprintf(want, sizeof want, "O(r)T(%.*s&y)C(r)", 2000, in + 3);
    doc(in, want);
}

static void esc(const char* in, int attr, const char* want)
{
    xb_t b;
    xb_init(&b);
    xml_escape(&b, in, strlen(in), attr);
    CHECK(b.n == strlen(want) && !memcmp(b.p, want, b.n), "escape(%s, attr=%d) -> %.*s, want %s", in, attr, (int)b.n,
        b.p ? (const char*)b.p : "", want);
    xb_free(&b);
}

static void test_escape(void)
{
    static char lng[400];
    esc("a<b>&\"c", 0, "a&lt;b&gt;&amp;\"c");
    esc("a\"b", 1, "a&quot;b");
    esc("t\tn\nr\r", 1, "t&#9;n&#10;r&#13;");
    esc("t\tn\n", 0, "t\tn\n");
    memset(lng, 'z', sizeof lng - 2);
    lng[sizeof lng - 2] = '&';
    lng[sizeof lng - 1] = 0;
    {
        xb_t b;
        xb_init(&b);
        xml_escape(&b, lng, strlen(lng), 0);
        CHECK(b.n == strlen(lng) + 4 && !memcmp(b.p + b.n - 5, "&amp;", 5), "escape finds & past the SIMD probe window");
        xb_free(&b);
    }
}

int main(void)
{
    simd_init();
    test_spec();
    test_long_text();
    test_escape();
    printf("test_xmlscan_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
