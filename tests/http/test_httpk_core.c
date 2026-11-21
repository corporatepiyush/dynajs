/*
 * test_httpk_core.c -- src/core/httpk, the HTTP/1.1 kit shared by dyna:http's
 * client and server, dyna:scrape, fetch and dyna:ai, as a standalone C17
 * library (no engine, no I/O).
 *
 * External oracles:
 *  - RFC 9110 5.5 field values: VCHAR / obs-text / SP / HTAB; NUL, CR, LF,
 *    other CTLs and DEL are refused. 5.6.2 tokens. 5.3: a list field's
 *    duplicates combine with ", "; a singleton (Content-Length, Host, ...)
 *    with two different values is an error; Set-Cookie never combines
 *    (joined with "\n" here so a caller can split it back).
 *  - RFC 9110 12.4.2 weight = OWS ";" OWS "q=" qvalue. One reading for the
 *    App server's Accept-Encoding and Negotiate()/NegotiateToken(), which
 *    had two parsers that disagreed: BWS around "=" is tolerated (the server
 *    pinned "gzip ; q = 0" -> identity); a q that is not a qvalue ("abc",
 *    "2", "1.5") is ignored, weight 1 (Negotiate used to drop "q=abc" and
 *    clamp "2"); digits past the third never turn a non-zero q into 0.
 *    Generated header values refuse every CTL including HTAB (the server's
 *    pinned contentType rule); received values allow HTAB (RFC 9110 5.5).
 *  - RFC 9110 5.6.7: the three HTTP-date forms of the 1994-11-06 example all
 *    mean 784111777, and a recipient MUST accept all three. (The retired
 *    crawlkit parser refused RFC 850 and asctime; that pin is reversed here.)
 *  - RFC 9112 4 status-line, pinned to the client's existing contract
 *    (tests/http/test_http_http_client_framing.js): exactly three digits, 000..599,
 *    HTTP/2 major accepted, LF-only lines refused, 1xx except 101 interim.
 *  - RFC 9112 6.1/6.3: Transfer-Encoding chunked counts only as the LAST
 *    coding; any other final coding reads to close; CL with TE is refused.
 *  - RFC 9112 7.1: chunk-size = 1*HEXDIG, BWS then ';' extension allowed.
 *    " 5", "+5", "0x5" and "5 x" are refused: dyna:scrape's getStream once
 *    parsed sizes with strtoll and accepted all three (finding B4). The
 *    decoder must give identical output when the body is split at every
 *    byte boundary.
 *  - Fetch standard 4.4 HTTP-redirect fetch: redirect statuses are exactly
 *    301/302/303/307/308; 303 (not HEAD) and 301/302 + POST become GET with
 *    no body; credentials go only to the same origin. A 3xx with no
 *    Location is not a redirect (finding B3: getStream/Crawler threw
 *    "more than N redirects" for a 304 at maxRedirects 0).
 *  - WHATWG HTML 9.2.6 event-stream interpretation, including its worked
 *    examples, CR / LF / CRLF line ends (CRLF split across two feeds), the
 *    leading BOM, "data" without a colon, and an unterminated last event
 *    that is never dispatched. Every input is re-fed one byte at a time.
 *  - RFC 6265 5.1.1 cookie-date, 5.1.3 domain-match, 5.1.4 path-match,
 *    5.3 storage (Domain equal to the host still makes a domain cookie;
 *    Max-Age beats Expires; Max-Age<=0 deletes), 5.4 Cookie header order
 *    (longer path first, then older); RFC 6265bis: Secure only from a
 *    secure origin, __Secure- / __Host- prefixes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/dyn-httpk-model.h"

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

#define S(x) x, sizeof(x) - 1

static void test_field(void)
{
    CHECK(httpk_name_ok(S("Content-Type")) && httpk_name_ok(S("x!#$%&'*+-.^_`|~1")), "tokens");
    CHECK(!httpk_name_ok(S("a b")) && !httpk_name_ok(S("a:b")) && !httpk_name_ok(S("")) && !httpk_name_ok(S("a\x80")), "non-tokens");
    CHECK(httpk_value_ok(S("a\tb c")) && httpk_value_ok(S("caf\xc3\xa9")) && httpk_value_ok(S("")), "HTAB, SP and obs-text are field content");
    CHECK(!httpk_value_ok(S("a\rb")) && !httpk_value_ok(S("a\nb")) && !httpk_value_ok("a\0b", 3)
            && !httpk_value_ok(S("a\x0b")) && !httpk_value_ok(S("a\x7f")),
        "CR, LF, NUL, VT, DEL refused");
    CHECK(httpk_list_has(S("keep-alive, Close"), "close") && !httpk_list_has(S("closed"), "close")
            && httpk_list_has(S(" , ,close ,"), "close"),
        "token lists");
    CHECK(httpk_list_last_is(S("gzip, chunked"), "chunked") && !httpk_list_last_is(S("chunked, gzip"), "chunked")
            && httpk_list_last_is(S("chunked ,"), "chunked"),
        "last coding");
    CHECK(httpk_qvalue(S("gzip;q=0"), "gzip") == 0 && httpk_qvalue(S("gzip; q=0.000"), "gzip") == 0
            && httpk_qvalue(S("gzip;q=0.0000"), "gzip") == 0,
        "q=0 forms");
    CHECK(httpk_qvalue(S("br, gzip;q=0.5"), "gzip") == 500 && httpk_qvalue(S("gzip"), "gzip") == 1000
            && httpk_qvalue(S("br"), "gzip") == -1 && httpk_qvalue(S("GZIP;Q=1"), "gzip") == 1000,
        "q values");
    CHECK(httpk_qvalue(S("gzip;q =0"), "gzip") == 0 && httpk_qvalue(S("gzip ; q = 0"), "gzip") == 0,
        "BWS around = is tolerated: the App server pinned \"gzip ; q = 0\" -> identity before httpk");
    CHECK(httpk_qvalue(S("gzip;q=0.0001"), "gzip") == 1 && httpk_qvalue(S("gzip;q=0.0010"), "gzip") == 1
            && httpk_qvalue(S("gzip;q=0.0005"), "gzip") == 1,
        "digits past the third never turn a non-zero weight into a refusal");
    CHECK(httpk_qvalue(S("gzip;q=abc"), "gzip") == 1000 && httpk_qvalue(S("gzip;q=2"), "gzip") == 1000
            && httpk_qvalue(S("gzip;q=1.5"), "gzip") == 1000 && httpk_qvalue(S("gzip;q"), "gzip") == 1000
            && httpk_qvalue(S("gzip;q="), "gzip") == 1000,
        "a q that is not an RFC 9110 12.4.2 qvalue is ignored: weight 1");
    CHECK(httpk_weight(S("text/html;level=1;q=0.3")) == 300 && httpk_weight(S("text/html")) == 1000
            && httpk_weight(S("en ;Q=0.25 ")) == 250 && httpk_weight(S("a;qq=0")) == 1000 && httpk_weight(NULL, 0) == 1000,
        "element weight");
    CHECK(httpk_value_gen_ok(S("text/plain; a=b")) && !httpk_value_gen_ok(S("a\tb")) && !httpk_value_gen_ok(S("a\x7f"))
            && httpk_value_ok(S("a\tb")),
        "generated values refuse every CTL incl. HTAB; received values allow HTAB");
    CHECK(httpk_field_kind(S("Content-Length")) == HTTPK_FIELD_SINGLETON && httpk_field_kind(S("set-cookie")) == HTTPK_FIELD_SET_COOKIE
            && httpk_field_kind(S("Accept")) == HTTPK_FIELD_LIST,
        "field kinds");
}

static int status_of(const char* line)
{
    int st = -2, minor;
    size_t ro, rl;
    if (httpk_status_line(line, strlen(line), &st, &minor, &ro, &rl) < 0)
        return -1;
    return st;
}

static int head(const char* raw, httpk_head_t* h, httpk_hdr_t* hv, uint32_t cap)
{
    size_t from = 0, he = httpk_head_end(raw, strlen(raw), &from);
    h->hdrs = hv;
    h->cap_hdrs = cap;
    if (!he)
        return -100;
    return httpk_resp_head(raw, he, h);
}

static void test_head(void)
{
    httpk_head_t h;
    httpk_hdr_t hv[16];
    dyn_sb_t b;
    size_t from = 0;
    const char* inc = "HTTP/1.1 200 OK\r\nA: b\r\n\r\nbody";
    size_t k, found = 0;

    for (k = 0; k <= strlen(inc) && !found; k++)
        found = httpk_head_end(inc, k, &from);
    CHECK(found == 25 && k == 26, "incremental head end finds the blank line once it arrives (%zu at %zu)", found, k);

    CHECK(status_of("HTTP/1.1 200 OK") == 200 && status_of("HTTP/1.0 204") == 204 && status_of("HTTP/2 200 OK") == 200, "status lines");
    CHECK(status_of("HTTP/1.1 099 Odd") == 99 && status_of("HTTP/1.1 599 x") == 599, "000..599");
    CHECK(status_of("HTTP/1.1 999 Odd") == -1 && status_of("HTTP/1.1 2000 W") == -1 && status_of("HTTP/1.1 20 W") == -1
            && status_of("HTTP/1.1 2xx W") == -1 && status_of("HTTP/1.1 200X") == -1 && status_of("HTTPS/1.1 200") == -1,
        "refused status lines");
    CHECK(httpk_is_interim(100) && httpk_is_interim(199) && !httpk_is_interim(101) && !httpk_is_interim(200), "interim");

    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &h, hv, 16) == HTTPK_OK && h.has_cl
            && h.content_length == 5,
        "equal duplicate CL");
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", &h, hv, 16) == HTTPK_E_CL_DUP, "conflicting CL");
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\n", &h, hv, 16) == HTTPK_E_CL
            && head("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n", &h, hv, 16) == HTTPK_E_CL,
        "malformed CL");
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length:  \r\n\r\n", &h, hv, 16) == HTTPK_E_CL_EMPTY, "empty CL");
    CHECK(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", &h, hv, 16) == HTTPK_E_CL_TE, "CL with TE");
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunkedx\r\n\r\n", &h, hv, 16) == HTTPK_OK
            && h.te_unframed && h.has_cl && h.content_length == 5,
        "CL with a non-chunked TE keeps CL framing (dyna:http request() and getStream pin it)");
    CHECK(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", &h, hv, 16) == HTTPK_OK && h.chunked && !h.te_unframed,
        "chunked last");
    CHECK(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n", &h, hv, 16) == HTTPK_OK && !h.chunked && h.te_unframed,
        "chunked not last: read to close");
    CHECK(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: xchunked\r\n\r\n", &h, hv, 16) == HTTPK_OK && !h.chunked,
        "xchunked is not the chunked coding");
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 42\r\ncontent-length:  42 \r\n\r\n", &h, hv, 16) == HTTPK_OK
            && h.content_length == 42,
        "an identical duplicate CL after OWS trimming");
    CHECK(head("HTTP/1.1 200 OK\r\nA: b\r\n folded\r\n\r\n", &h, hv, 16) == HTTPK_E_FOLD, "obs-fold");
    CHECK(head("HTTP/1.1 200 OK\nA: b\n\n\r\n\r\n", &h, hv, 16) == HTTPK_E_STATUS, "LF-only refused");
    CHECK(head("HTTP/1.1 200 OK\r\nA : b\r\n\r\n", &h, hv, 16) == HTTPK_E_LINE, "space before colon");
    {
        char nul[] = "HTTP/1.1 200 OK\r\nA: b?c\r\n\r\n";
        nul[22] = 0;
        h.hdrs = hv;
        h.cap_hdrs = 16;
        CHECK(httpk_resp_head(nul, sizeof nul - 1, &h) == HTTPK_E_LINE, "NUL in a value");
    }
    CHECK(head("HTTP/1.1 200 OK\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", &h, hv, 2) == HTTPK_E_TOO_MANY, "header cap");
    CHECK(head("HTTP/1.0 200 OK\r\n\r\n", &h, hv, 16) == HTTPK_OK && h.conn_close, "1.0 closes by default");
    CHECK(head("HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\n\r\n", &h, hv, 16) == HTTPK_OK && h.conn_close, "close token");

    {
        const char* r = "HTTP/1.1 200 OK\r\nAccept: a\r\nX: 1\r\naccept: b\r\nHost: h\r\nhost: h\r\nSet-Cookie: a=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT\r\nset-cookie: b=2\r\n\r\n";
        CHECK(head(r, &h, hv, 16) == HTTPK_OK, "parse");
        dyn_sb_init(&b);
        CHECK(httpk_hdr_value(&h, r, "accept", &b) == HTTPK_HV_COMBINED && b.n == 4 && memcmp(b.p, "a, b", 4) == 0, "list combine");
        b.n = 0;
        CHECK(httpk_hdr_value(&h, r, "host", &b) == HTTPK_HV_ONE && b.n == 1, "identical singleton duplicates are one value");
        b.n = 0;
        CHECK(httpk_hdr_value(&h, r, "set-cookie", &b) == HTTPK_HV_COMBINED && b.n == 46
                && memcmp(b.p + 42, "\nb=2", 4) == 0,
            "Set-Cookie never joins with a comma");
        b.n = 0;
        CHECK(httpk_hdr_value(&h, r, "missing", &b) == HTTPK_HV_NONE, "absent");
        dyn_sb_free(&b);
    }
    {
        const char* r = "HTTP/1.1 302 Found\r\nLocation: /a\r\nX: y\r\nlocation: /b\r\n\r\n";
        CHECK(head(r, &h, hv, 16) == HTTPK_E_DUP && h.dup_field == 2,
            "RFC 9110 5.3: a conflicting singleton fails the head, naming the field (not first/last wins)");
        CHECK(head("HTTP/1.1 200 OK\r\nContent-Type: a/b\r\ncontent-type: a/b\r\nVary: x\r\nVary: y\r\n\r\n", &h, hv, 16) == HTTPK_OK,
            "identical singleton duplicates and repeated list fields parse");
        CHECK(head("HTTP/1.1 200 OK\r\nDate: a\r\nETag: \"1\"\r\nDate: b\r\n\r\n", &h, hv, 16) == HTTPK_E_DUP && h.dup_field == 2,
            "Date is a singleton");
    }
    CHECK(httpk_bodyless(204, "GET") && httpk_bodyless(304, "GET") && httpk_bodyless(200, "HEAD") && !httpk_bodyless(200, "GET"), "bodyless");
}

static int dechunk_all(const char* in, size_t n, size_t step, char* out, size_t* outn, size_t* used)
{
    httpk_dechunk_t d;
    uint8_t* buf = (uint8_t*)malloc(n + 1);
    size_t pos = 0, total = 0;
    int rc = HTTPK_DC_MORE;
    httpk_dechunk_init(&d);
    memcpy(buf, in, n);
    while (pos < n && rc == HTTPK_DC_MORE) {
        size_t take = n - pos < step ? n - pos : step, c = 0, o = 0;
        rc = httpk_dechunk(&d, buf + pos, take, &c, &o);
        if (rc == HTTPK_DC_BAD)
            break;
        memcpy(out + total, buf + pos, o);
        total += o;
        pos += c;
        if (c < take && rc != HTTPK_DC_DONE)
            break;
    }
    *outn = total;
    *used = pos;
    free(buf);
    return rc;
}

static void test_chunk(void)
{
    static const char body[] = "5\r\nhello\r\n6;ext=1\r\n world\r\na \t;x\r\n0123456789\r\n0\r\nTrailer: x\r\n\r\nNEXT";
    char out[128];
    size_t on, used, step;
    uint64_t sz;
    int rc;

    rc = dechunk_all(body, sizeof body - 1, sizeof body, out, &on, &used);
    CHECK(rc == HTTPK_DC_DONE && on == 21 && memcmp(out, "hello world0123456789", 21) == 0 && used == sizeof body - 5,
        "whole body (rc %d on %zu used %zu)", rc, on, used);
    for (step = 1; step < sizeof body; step++) {
        rc = dechunk_all(body, sizeof body - 1, step, out, &on, &used);
        if (rc != HTTPK_DC_DONE || on != 21 || memcmp(out, "hello world0123456789", 21) != 0) {
            CHECK(0, "split every %zu bytes", step);
            break;
        }
    }
    CHECK(step == sizeof body, "every split size decodes identically");
    {
        httpk_dechunk_t d;
        char tb[] = "0\r\nA: b\r\nCc: dd\r\n\r\n";
        size_t c = 0, o = 0;
        httpk_dechunk_init(&d);
        rc = httpk_dechunk(&d, (uint8_t*)tb, sizeof tb - 1, &c, &o);
        CHECK(rc == HTTPK_DC_DONE && d.trailer == 14 && o == 0, "trailer bytes counted for the caller's budget (%llu)",
            (unsigned long long)d.trailer);
    }

    CHECK(dechunk_all(S(" 5\r\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "leading space refused");
    CHECK(dechunk_all(S("+5\r\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "sign refused");
    CHECK(dechunk_all(S("0x5\r\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "0x prefix refused");
    CHECK(dechunk_all(S("5 x\r\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "junk after BWS refused");
    CHECK(dechunk_all(S("5\r\nhelloXX0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "data not followed by CRLF");
    CHECK(dechunk_all(S("5\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "bare LF size line");
    CHECK(dechunk_all(S("10000000000000000\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "17 hex digits overflow");
    CHECK(dechunk_all(S("1ffffffffffff\r\n"), 64, out, &on, &used) == HTTPK_DC_MORE
            && dechunk_all(S("0001ffffffffffff\r\n"), 64, out, &on, &used) == HTTPK_DC_MORE,
        "13 significant hex digits fit (sizes stay below 2^53, exact as a JS number)");
    CHECK(dechunk_all(S("1fffffffffffff\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD,
        "14 significant digits overflow (the App client's pinned bound)");
    CHECK(dechunk_all(S("00000000000000000005\r\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_DONE && on == 5,
        "leading zeros are padding (RFC 9112 7.1: 1*HEXDIG; the App client pinned 20-digit sizes)");
    CHECK(dechunk_all(S("0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                        "00000000000000000000000000000000000000000000000000000000005\r\n"),
              64, out, &on, &used)
            == HTTPK_DC_BAD,
        "a size line of 255+ digits is refused");
    CHECK(dechunk_all(S("0\r\n\nX: y\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD
            && dechunk_all("0\r\n\0X: y\r\n\r\n", 13, 64, out, &on, &used) == HTTPK_DC_BAD,
        "a trailer line starting with bare LF or NUL is refused like one containing them (RFC 9112 7.1.2; it used to end the body)");
    CHECK(dechunk_all(S("5;a\x01\r\nhello\r\n0\r\n\r\n"), 64, out, &on, &used) == HTTPK_DC_BAD, "control in extension");
    CHECK(dechunk_all(S("5\r\nhel"), 64, out, &on, &used) == HTTPK_DC_MORE && on == 3, "truncated stays MORE with partial data");
    CHECK(httpk_chunk_size(S("1A;x"), &sz) == 0 && sz == 26 && httpk_chunk_size(S("0"), &sz) == 0 && sz == 0, "size helper");
    CHECK(httpk_chunk_size(S("0x1"), &sz) < 0 && httpk_chunk_size(S(""), &sz) < 0 && httpk_chunk_size(S("-1"), &sz) < 0, "size helper refusals");
}

static void test_date(void)
{
    CHECK(httpk_http_date(S("Sun, 06 Nov 1994 08:49:37 GMT")) == 784111777, "IMF-fixdate");
    CHECK(httpk_http_date(S("Sunday, 06-Nov-94 08:49:37 GMT")) == 784111777, "RFC 850");
    CHECK(httpk_http_date(S("Sun Nov  6 08:49:37 1994")) == 784111777, "asctime");
    CHECK(httpk_http_date(S("Sunday, 06-Nov-05 08:49:37 GMT")) == httpk_http_date(S("Sun, 06 Nov 2005 08:49:37 GMT"))
            && httpk_http_date(S("Sun, 06 Nov 2005 08:49:37 GMT")) > 0,
        "RFC 850 two-digit year below 70 is 20xx");
    CHECK(httpk_http_date(S("Thu, 01 Jan 1970 00:00:00 GMT")) == 0 && httpk_http_date(S("Tue, 29 Feb 2000 23:59:60 GMT")) == 951868800,
        "epoch, leap day, leap second");
    CHECK(httpk_http_date(S("Sun, 06 Nov 1994 24:00:00 GMT")) == -1 && httpk_http_date(S("Sun, 06 Foo 1994 08:49:37 GMT")) == -1
            && httpk_http_date(S("Sun, 31 Feb 1994 08:49:37 GMT")) == -1 && httpk_http_date(S("garbage")) == -1,
        "refusals");
    CHECK(httpk_cookie_date(S("Wed, 21 Oct 2015 07:28:00 GMT")) == 1445412480, "cookie date IMF");
    CHECK(httpk_cookie_date(S("Wednesday, 21-Oct-15 07:28:00 GMT")) == 1445412480, "cookie date 2-digit year");
    CHECK(httpk_cookie_date(S("21 Oct 2015 7:28:0")) == 1445412480, "cookie date lenient token order");
    CHECK(httpk_http_date(S("Wed, 29 Feb 2023 00:00:00 GMT")) == -1 && httpk_http_date(S("Wednesday, 29-Feb-23 00:00:00 GMT")) == -1
            && httpk_http_date(S("Wed Feb 29 00:00:00 2023")) == -1 && httpk_http_date(S("Thu, 29 Feb 1900 00:00:00 GMT")) == -1
            && httpk_cookie_date(S("29 Feb 2023 00:00:00")) == -1 && httpk_cookie_date(S("29 Feb 2100 00:00:00")) == -1,
        "29 Feb of a common year is no date (RFC 9110 5.6.7, RFC 6265 5.1.1 step 5; it used to read as 1 Mar)");
    CHECK(httpk_http_date(S("Thu, 29 Feb 2024 00:00:00 GMT")) == 1709164800 && httpk_cookie_date(S("29 Feb 2400 00:00:00")) > 0,
        "29 Feb of a leap year (divisible by 4, by 400 across centuries)");
    CHECK(httpk_cookie_date(S("Wed, 32 Oct 2015 07:28:00 GMT")) == -1 && httpk_cookie_date(S("Oct 2015")) == -1, "cookie date refusals");
    CHECK(httpk_delta_seconds(S(" 120 ")) == 120.0 && httpk_delta_seconds(S("1.5")) < 0 && httpk_delta_seconds(S("")) < 0, "delta-seconds");
}

static void test_fixdate(void)
{
    char b[HTTPK_IMF_FIXDATE_LEN + 1];
    const char* l;
    int64_t t;
    uint64_t n0;
    int bad = 0;
    b[HTTPK_IMF_FIXDATE_LEN] = 0;
    CHECK(httpk_imf_fixdate(784111777, b) == HTTPK_IMF_FIXDATE_LEN && strcmp(b, "Sun, 06 Nov 1994 08:49:37 GMT") == 0, "fixdate RFC 9110 example");
    CHECK(httpk_imf_fixdate(0, b) && strcmp(b, "Thu, 01 Jan 1970 00:00:00 GMT") == 0, "fixdate epoch");
    CHECK(httpk_imf_fixdate(951868799, b) && strcmp(b, "Tue, 29 Feb 2000 23:59:59 GMT") == 0, "fixdate leap day");
    CHECK(httpk_imf_fixdate(-1, b) && strcmp(b, "Wed, 31 Dec 1969 23:59:59 GMT") == 0, "fixdate before epoch");
    CHECK(httpk_imf_fixdate(253402300799LL, b) && strcmp(b, "Fri, 31 Dec 9999 23:59:59 GMT") == 0, "fixdate year 9999");
    CHECK(httpk_imf_fixdate(-11644473600LL, b) && strcmp(b, "Mon, 01 Jan 1601 00:00:00 GMT") == 0, "fixdate year 1601");
    CHECK(httpk_imf_fixdate(253402300800LL, b) == 0 && httpk_imf_fixdate(-11644473601LL, b) == 0, "fixdate out of range");
    for (t = -11644473600LL; t <= 253402300799LL; t += 86399 * 37 + 12345)
        if (!httpk_imf_fixdate(t, b) || httpk_http_date(b, HTTPK_IMF_FIXDATE_LEN) != t)
            bad++;
    CHECK(bad == 0, "fixdate round-trips through httpk_http_date across 1601..9999");
    n0 = httpk_date_line_builds();
    l = httpk_date_line(784111777);
    CHECK(l && strlen(l) == HTTPK_DATE_LINE_LEN && strcmp(l, "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n") == 0, "date line");
    CHECK(httpk_date_line(784111777) == l && httpk_date_line_builds() == n0 + 1, "same second reuses the cached line");
    l = httpk_date_line(784111778);
    CHECK(strcmp(l, "Date: Sun, 06 Nov 1994 08:49:38 GMT\r\n") == 0 && httpk_date_line_builds() == n0 + 2, "next second rebuilds once");
    CHECK(httpk_date_line(784111778) && httpk_date_line_builds() == n0 + 2, "still cached");
    CHECK(httpk_date_line(253402300800LL) == NULL, "date line out of range");
    CHECK(httpk_block_has(S("X-A: 1\r\ndate: x\r\n"), "date") && httpk_block_has(S("DATE:x"), "date")
            && !httpk_block_has(S("X-Date: 1\r\n"), "date") && !httpk_block_has(S("Dates: 1\r\n"), "date")
            && !httpk_block_has(S("X: date: 1\r\n"), "date") && !httpk_block_has(S(""), "date"),
        "header block name lookup");
}

static void test_retry(void)
{
    uint64_t rng = 42;
    int i, okb = 1;
    CHECK(httpk_retry_status(429, HTTPK_RETRY_SCRAPE) && httpk_retry_status(503, HTTPK_RETRY_SCRAPE) && !httpk_retry_status(408, HTTPK_RETRY_SCRAPE),
        "scrape policy");
    CHECK(httpk_retry_status(408, HTTPK_RETRY_API) && httpk_retry_status(409, HTTPK_RETRY_API) && httpk_retry_status(529, HTTPK_RETRY_API)
            && !httpk_retry_status(400, HTTPK_RETRY_API) && !httpk_retry_status(404, HTTPK_RETRY_API),
        "API policy (Anthropic 529 overloaded)");
    for (i = 0; i < 12; i++) {
        double b = httpk_backoff_ms(100, 0, i, &rng), ideal = 100.0 * (double)(1 << (i < 10 ? i : 10));
        if (b < ideal * 0.75 || b > ideal * 1.25)
            okb = 0;
    }
    CHECK(okb, "jitter within +-25%% of base*2^min(attempt,10)");
    CHECK(httpk_backoff_ms(100, 250, 9, &rng) == 250, "cap");
    CHECK(httpk_retry_after_ms(S("3"), NULL, 0, 0) == 3000.0, "Retry-After seconds");
    CHECK(httpk_retry_after_ms(S("3"), S("1500.5"), 0) == 1500.5, "retry-after-ms wins");
    CHECK(httpk_retry_after_ms(S("Sun, 06 Nov 1994 08:49:47 GMT"), NULL, 0, 784111777) == 10000.0, "Retry-After date");
    CHECK(httpk_retry_after_ms(S("Sun, 06 Nov 1994 08:49:37 GMT"), NULL, 0, 784111800) == 0.0, "past date");
    CHECK(httpk_retry_after_ms(S("soon"), S("x"), 0) < 0 && httpk_retry_after_ms(NULL, 0, NULL, 0, 0) < 0, "unusable");
}

static void test_url_redirect(void)
{
    httpk_url_t u;
    httpk_redirect_t r;
    const char* x = "HTTPS://user@[::1]:8443/p/a?q=1#f";
    CHECK(httpk_url_split(x, strlen(x), &u) == 0 && u.https && u.ipv6 && u.port == 8443 && u.host_len == 5
            && memcmp(x + u.host, "[::1]", 5) == 0 && u.path_len == 4 && u.query_len == 3,
        "split");
    CHECK(httpk_url_split(S("http://h"), &u) == 0 && u.path_len == 0 && u.port == 80 && u.default_port, "no path");
    CHECK(httpk_url_split(S("ftp://h/"), &u) < 0 && httpk_url_split(S("http://h:0/"), &u) < 0 && httpk_url_split(S("http://h:65536/"), &u) < 0
            && httpk_url_split(S("http:///x"), &u) < 0 && httpk_url_split(S("http://a b/"), &u) < 0,
        "refused");
    CHECK(httpk_same_origin("https://A.example/x", "https://a.example:443/y") && !httpk_same_origin("https://a.example/", "http://a.example/")
            && !httpk_same_origin("https://a.example/", "https://b.example/"),
        "origins");

    CHECK(httpk_redirect_plan(303, "POST", "http://a/", "http://a/b", 0, 5, 0, &r) == HTTPK_REDIR_FOLLOW && !strcmp(r.method, "GET")
            && r.drop_body && !r.strip_credentials,
        "303 POST -> GET");
    CHECK(httpk_redirect_plan(303, "HEAD", "http://a/", "http://a/b", 0, 5, 0, &r) == HTTPK_REDIR_FOLLOW && !strcmp(r.method, "HEAD"), "303 HEAD stays");
    CHECK(httpk_redirect_plan(301, "POST", "http://a/", "http://a/b", 0, 5, 0, &r) == HTTPK_REDIR_FOLLOW && !strcmp(r.method, "GET"), "301 POST");
    CHECK(httpk_redirect_plan(302, "PUT", "http://a/", "http://a/b", 0, 5, 0, &r) == HTTPK_REDIR_FOLLOW && !strcmp(r.method, "PUT") && !r.drop_body,
        "302 PUT keeps its method (fetch rewrites only POST)");
    CHECK(httpk_redirect_plan(307, "POST", "http://a/", "http://b/", 0, 5, 0, &r) == HTTPK_REDIR_FOLLOW && !strcmp(r.method, "POST")
            && r.strip_credentials,
        "307 keeps method, cross-origin strips credentials");
    CHECK(httpk_redirect_plan(307, "GET", "http://a/", "http://b/", 0, 5, HTTPK_REDIR_KEEP_CREDENTIALS, &r) == HTTPK_REDIR_FOLLOW
            && !r.strip_credentials,
        "opt-in keep");
    CHECK(httpk_redirect_plan(301, "GET", "https://a/", "http://a/", 0, 5, 0, &r) == HTTPK_REDIR_E_DOWNGRADE
            && httpk_redirect_plan(301, "GET", "https://a/", "http://a/", 0, 5, HTTPK_REDIR_ALLOW_DOWNGRADE, &r) == HTTPK_REDIR_FOLLOW,
        "downgrade");
    CHECK(httpk_redirect_plan(304, "GET", "http://a/", NULL, 0, 0, 0, &r) == HTTPK_REDIR_NONE
            && httpk_redirect_plan(302, "GET", "http://a/", NULL, 9, 0, 0, &r) == HTTPK_REDIR_NONE
            && httpk_redirect_plan(300, "GET", "http://a/", "http://a/b", 0, 5, 0, &r) == HTTPK_REDIR_NONE,
        "no Location / 304 / 300 are not redirects, even past the hop limit");
    CHECK(httpk_redirect_plan(302, "GET", "http://a/", "http://a/b", 5, 5, 0, &r) == HTTPK_REDIR_E_TOO_MANY, "hop limit");
    CHECK(httpk_redirect_plan(302, "GET", "http://a/", "mailto:x", 0, 5, 0, &r) == HTTPK_REDIR_E_SCHEME, "scheme");
    CHECK(httpk_redirect_plan(302, "GET", "http://a/", "http://a/", 0, 5, 0, &r) == HTTPK_REDIR_E_LOOP, "self loop");
    CHECK(httpk_is_credential_name(S("Authorization")) && httpk_is_credential_name(S("cookie")) && !httpk_is_credential_name(S("accept")),
        "credential names");
}

typedef struct {
    char log[2048];
    size_t n;
    int events;
    int stop_at;
} sse_rec_t;

static int sse_cb(void* ud, const httpk_sse_event_t* ev)
{
    sse_rec_t* r = (sse_rec_t*)ud;
    int w = snprintf(r->log + r->n, sizeof r->log - r->n, "[%.*s|%.*s|%.*s|%lld]", (int)ev->type_len, ev->type,
        (int)ev->data_len, ev->data, (int)ev->id_len, ev->id, (long long)ev->retry_ms);
    if (w > 0)
        r->n += (size_t)w;
    r->events++;
    return r->stop_at && r->events >= r->stop_at;
}

static void sse_run(const char* in, size_t n, size_t step, sse_rec_t* r, int* rc)
{
    httpk_sse_t p;
    size_t i;
    memset(r, 0, sizeof *r);
    httpk_sse_init(&p, sse_cb, r, 0);
    *rc = 0;
    for (i = 0; i < n && !*rc; i += step)
        *rc = httpk_sse_feed(&p, in + i, n - i < step ? n - i : step);
    httpk_sse_free(&p);
}

static void sse_case(const char* in, size_t n, const char* want, const char* what)
{
    sse_rec_t r;
    int rc;
    size_t step;
    sse_run(in, n, n ? n : 1, &r, &rc);
    CHECK(rc == 0 && strcmp(r.log, want) == 0, "%s: got %s want %s", what, r.log, want);
    for (step = 1; step < n; step++) {
        sse_run(in, n, step, &r, &rc);
        if (rc != 0 || strcmp(r.log, want) != 0) {
            CHECK(0, "%s split %zu: %s", what, step, r.log);
            return;
        }
    }
}

static void test_sse(void)
{
    dyn_sb_t b;
    sse_rec_t r;
    int rc;
    sse_case(S("data: YHOO\ndata: +2\ndata: 10\n\n"), "[message|YHOO\n+2\n10||-1]", "WHATWG stock example");
    sse_case(S(": test stream\n\ndata: first event\nid: 1\n\ndata:second event\nid\n\ndata:  third event\n\n"),
        "[message|first event|1|-1][message|second event||-1][message| third event||-1]", "WHATWG id example");
    sse_case(S("data\n\ndata\ndata\n\ndata:\n"), "[message|||-1][message|\n||-1]", "WHATWG empty data example; last unterminated");
    sse_case(S("event: message_start\r\ndata: {\"a\":1}\r\n\r\nevent: ping\rdata: {}\r\r"),
        "[message_start|{\"a\":1}||-1][ping|{}||-1]", "CRLF and CR terminators");
    sse_case(S("\xef\xbb\xbf"
               "data: x\n\n"),
        "[message|x||-1]", "leading BOM");
    sse_case(S("retry: 2500\nretry: x\nevent: e\nid: 7\n\nevent: f\n\ndata: y\n\n"), "[message|y|7|2500]",
        "a bad retry is ignored; an event without data is not dispatched, resets the type, and the pending retry rides on the next dispatched event");
    sse_case(S("retry: 2500\ndata: z\n\n"), "[message|z||2500]", "retry delivered with the next event");
    sse_case(S("id: a\0b\ndata: q\n\n"), "[message|q||-1]", "id with NUL ignored");

    memset(&r, 0, sizeof r);
    {
        httpk_sse_t p;
        httpk_sse_init(&p, sse_cb, &r, 16);
        rc = httpk_sse_feed(&p, S("data: 0123456789abcdefghij\n\n"));
        CHECK(rc == HTTPK_SSE_E_TOO_BIG, "event size cap");
        httpk_sse_free(&p);
    }
    {
        httpk_sse_t p;
        memset(&r, 0, sizeof r);
        r.stop_at = 1;
        httpk_sse_init(&p, sse_cb, &r, 0);
        rc = httpk_sse_feed(&p, S("data: 1\n\ndata: 2\n\n"));
        CHECK(rc == HTTPK_SSE_E_ABORT && r.events == 1 && httpk_sse_feed(&p, S("data: 3\n\n")) == HTTPK_SSE_E_ABORT, "callback abort sticks");
        httpk_sse_free(&p);
    }
    dyn_sb_init(&b);
    CHECK(httpk_sse_frame(&b, S("delta"), S("9"), S("a\nb\r\nc\rd"), 1000) == 0, "frame");
    sse_run(b.p, b.n, b.n, &r, &rc);
    CHECK(rc == 0 && strcmp(r.log, "[delta|a\nb\nc\nd|9|1000]") == 0, "frame round-trips through the parser: %s", r.log);
    b.n = 0;
    CHECK(httpk_sse_frame(&b, S("a\nb"), NULL, 0, S("x"), -1) < 0, "newline in event name refused");
    dyn_sb_free(&b);
}

static const char* jar_hdr(httpk_jar_t* j, const char* url, int64_t now)
{
    static char out[512];
    dyn_sb_t b;
    dyn_sb_init(&b);
    httpk_jar_header(j, url, now, &b);
    snprintf(out, sizeof out, "%.*s", (int)b.n, b.p ? b.p : "");
    dyn_sb_free(&b);
    return out;
}

static void test_cookie(void)
{
    httpk_jar_t j;
    const int64_t now = 1445412480;
    httpk_jar_init(&j, 0);
    CHECK(httpk_jar_set(&j, "http://www.example.com/", S("SID=31d4d96e407aad42; Path=/; Domain=example.com"), now) == HTTPK_CK_STORED, "RFC 6265 3.1 SID");
    CHECK(!strcmp(jar_hdr(&j, "http://www.example.com/", now), "SID=31d4d96e407aad42"), "sent back");
    CHECK(!strcmp(jar_hdr(&j, "http://sub.www.example.com/x", now), "SID=31d4d96e407aad42"), "domain cookie reaches subdomains");
    CHECK(!strcmp(jar_hdr(&j, "http://example.org/", now), ""), "not to other domains");
    CHECK(httpk_jar_set(&j, "http://www.example.com/", S("lang=en-US; Path=/; Domain=example.com"), now) == HTTPK_CK_STORED, "lang");
    CHECK(!strcmp(jar_hdr(&j, "http://www.example.com/", now), "SID=31d4d96e407aad42; lang=en-US"), "two cookies, creation order");
    CHECK(httpk_jar_set(&j, "http://www.example.com/", S("lang=; Expires=Sun, 06 Nov 1994 08:49:37 GMT"), now) == HTTPK_CK_DELETED
            && !strcmp(jar_hdr(&j, "http://www.example.com/", now), "SID=31d4d96e407aad42; lang=en-US"),
        "an already-expired host-only cookie is not stored and does not touch the domain cookie of the same name");
    CHECK(httpk_jar_set(&j, "http://www.example.com/", S("lang=; Path=/; Domain=example.com; Max-Age=0"), now) == HTTPK_CK_DELETED, "Max-Age=0 deletes");
    CHECK(!strcmp(jar_hdr(&j, "http://www.example.com/", now), "SID=31d4d96e407aad42"), "deleted");
    CHECK(httpk_jar_set(&j, "http://h.test/docs/a/page", S("p=1"), now) == HTTPK_CK_STORED && !strcmp(jar_hdr(&j, "http://h.test/docs/a/x", now), "p=1")
            && !strcmp(jar_hdr(&j, "http://h.test/docs/", now), ""),
        "default-path is the directory of the request path");
    CHECK(httpk_jar_set(&j, "http://h.test/", S("q=2; Path=/docs"), now) == HTTPK_CK_STORED && !strcmp(jar_hdr(&j, "http://h.test/docs/a/x", now), "p=1; q=2")
            && !strcmp(jar_hdr(&j, "http://h.test/docsx", now), ""),
        "longer path first; /docs does not path-match /docsx");
    CHECK(!strcmp(jar_hdr(&j, "http://sub.h.test/docs/a/x", now), ""), "host-only cookie stays on its host");
    CHECK(httpk_jar_set(&j, "http://h.test/", S("s=1; Secure"), now) == HTTPK_CK_IGNORED, "Secure from http ignored");
    CHECK(httpk_jar_set(&j, "https://h.test/", S("s=1; Secure"), now) == HTTPK_CK_STORED && !strstr(jar_hdr(&j, "http://h.test/", now), "s=1")
            && strstr(jar_hdr(&j, "https://h.test/", now), "s=1"),
        "secure only over https");
    CHECK(httpk_jar_set(&j, "http://h.test/", S("s=2"), now) == HTTPK_CK_IGNORED, "http cannot overwrite a Secure cookie");
    CHECK(httpk_jar_set(&j, "http://a.example.com/", S("x=1; Domain=b.example.com"), now) == HTTPK_CK_IGNORED, "foreign Domain ignored");
    CHECK(httpk_jar_set(&j, "http://a.example.com/", S("x=1; Domain=com"), now) == HTTPK_CK_IGNORED, "single-label Domain ignored");
    CHECK(httpk_jar_set(&j, "http://h.test/", S("__Secure-a=1"), now) == HTTPK_CK_IGNORED
            && httpk_jar_set(&j, "https://h.test/", S("__Host-a=1; Secure; Path=/; Domain=h.test"), now) == HTTPK_CK_IGNORED
            && httpk_jar_set(&j, "https://h.test/", S("__Host-a=1; Secure; Path=/"), now) == HTTPK_CK_STORED,
        "prefixes");
    CHECK(httpk_jar_set(&j, "http://t.test/", S("e=1; Max-Age=60; Expires=Thu, 01 Jan 1970 00:00:00 GMT"), now) == HTTPK_CK_STORED
            && !strcmp(jar_hdr(&j, "http://t.test/", now + 59), "e=1") && !strcmp(jar_hdr(&j, "http://t.test/", now + 61), ""),
        "Max-Age beats Expires and expires");
    CHECK(httpk_jar_set(&j, "http://t.test/", S("bad=a\x01"), now) == HTTPK_CK_IGNORED && httpk_jar_set(&j, "http://t.test/", S(";"), now) == HTTPK_CK_IGNORED,
        "control bytes / empty pair");
    httpk_jar_free(&j);
    httpk_jar_init(&j, 2);
    httpk_jar_set(&j, "http://c.test/", S("a=1"), now);
    httpk_jar_set(&j, "http://c.test/", S("b=1"), now);
    httpk_jar_set(&j, "http://c.test/", S("c=1"), now);
    CHECK(j.n == 2 && !strcmp(jar_hdr(&j, "http://c.test/", now), "b=1; c=1"), "cap evicts the oldest");
    httpk_jar_free(&j);
    httpk_jar_init(&j, 8);
    CHECK(httpk_jar_set(&j, "http://c.test/", S("a=1; Path=/\0x"), now) == HTTPK_CK_IGNORED
            && httpk_jar_set(&j, "http://c.test/", S("b=1; Secure\x7f"), now) == HTTPK_CK_IGNORED
            && httpk_jar_set(&j, "http://c.test/", S("c=1; Path=/\nX"), now) == HTTPK_CK_IGNORED
            && httpk_jar_set(&j, "http://c.test/", S("d=1;\tPath=/"), now) == HTTPK_CK_STORED && j.n == 1,
        "a CTL other than HTAB anywhere in the line ignores it (RFC 6265bis 5.6 step 1; a NUL in an attribute used to store)");
    httpk_jar_free(&j);
}

static void test_req(void)
{
    dyn_sb_t b;
    dyn_sb_init(&b);
    CHECK(httpk_req_line(&b, "GET", "/a b\x7f") == 0 && b.n == 24 && memcmp(b.p, "GET /a%20b%7F HTTP/1.1\r\n", 24) == 0, "target CTL escaping");
    b.n = 0;
    CHECK(httpk_req_line(&b, "GE T", "/") < 0 && httpk_req_line(&b, "", "/") < 0, "bad method");
    b.n = 0;
    CHECK(httpk_req_header(&b, S("X-A"), S("v\tw")) == 0 && httpk_req_header(&b, S("X A"), S("v")) < 0
            && httpk_req_header(&b, S("X-B"), S("v\r\nInjected: 1")) < 0,
        "header validation");
    CHECK(httpk_is_framing_name(S("Content-Length")) && httpk_is_framing_name(S("HOST")) && !httpk_is_framing_name(S("Accept")), "framing names");
    dyn_sb_free(&b);
}

static unsigned long hk_alloc_n, hk_fail_at;
static int hk_failed;

#undef malloc
#undef realloc
void* malloc(size_t n);
void* realloc(void* p, size_t n);

void* hk_t_malloc(size_t n)
{
    if (hk_fail_at && ++hk_alloc_n == hk_fail_at) {
        hk_failed = 1;
        return NULL;
    }
    return malloc(n);
}

void* hk_t_realloc(void* p, size_t n)
{
    if (hk_fail_at && ++hk_alloc_n == hk_fail_at) {
        hk_failed = 1;
        return NULL;
    }
    return realloc(p, n);
}

static int hk_bad(int c, const char* op, unsigned long at, int rc)
{
    if (c)
        printf("  alloc failure: %s with allocation %lu failing returned %d\n", op, at, rc);
    return c;
}

static void hk_arm(unsigned long at)
{
    hk_alloc_n = 0;
    hk_failed = 0;
    hk_fail_at = at;
}

static int hk_sse_nop(void* ud, const httpk_sse_event_t* ev)
{
    (void)ud;
    (void)ev;
    return 0;
}

static void test_alloc_failure(void)
{
    static const char sse[] = "event: e\nid: 7\ndata: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\ndata: b\n\n";
    static const char head[] = "HTTP/1.1 200 OK\r\nVia: a\r\nVia: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r\n\r\n";
    unsigned long at;
    int done_set = 0, done_hdr = 0, done_sse = 0, done_hv = 0, bad = 0;
    for (at = 1; at < 200 && !(done_set && done_hdr && done_sse && done_hv); at++) {
        httpk_jar_t j;
        dyn_sb_t sb, before;
        httpk_sse_t ps;
        httpk_hdr_t hv[8];
        httpk_head_t h;
        int rc;
        httpk_jar_init(&j, 8);
        dyn_sb_init(&before);
        httpk_jar_set(&j, "http://a.test/x/y", S("old=1; Path=/"), 1000);
        httpk_jar_header(&j, "http://a.test/x/y", 1000, &before);
        if (!done_set) {
            hk_arm(at);
            rc = httpk_jar_set(&j, "http://a.test/x/y", S("sid=12345678901234567890; Domain=a.test; Path=/x"), 1000);
            hk_fail_at = 0;
            if (hk_failed) {
                dyn_sb_init(&sb);
                httpk_jar_header(&j, "http://a.test/x/y", 1000, &sb);
                bad += hk_bad(rc != HTTPK_CK_OOM || j.n != 1 || sb.n != before.n || memcmp(sb.p, before.p, sb.n), "jar_set", at, rc);
                dyn_sb_free(&sb);
            } else {
                bad += hk_bad(rc != HTTPK_CK_STORED || j.n != 2, "jar_set ok", at, rc);
                done_set = 1;
            }
        }
        if (!done_hdr) {
            dyn_sb_init(&sb);
            hk_arm(at);
            rc = httpk_jar_header(&j, "http://a.test/x/y", 1000, &sb);
            hk_fail_at = 0;
            if (hk_failed)
                bad += hk_bad(rc != -1 || !sb.oom, "jar_header", at, rc);
            else
                bad += hk_bad(rc < 1, "jar_header ok", at, rc), done_hdr = 1;
            dyn_sb_free(&sb);
        }
        if (!done_sse) {
            int k;
            rc = 0;
            httpk_sse_init(&ps, hk_sse_nop, NULL, 0);
            hk_arm(at);
            for (k = 0; k < (int)sizeof sse - 1 && rc == 0; k += 7)
                rc = httpk_sse_feed(&ps, sse + k, (size_t)(sizeof sse - 1 - (size_t)k) < 7 ? sizeof sse - 1 - (size_t)k : 7);
            hk_fail_at = 0;
            if (hk_failed)
                bad += hk_bad(rc != HTTPK_SSE_E_OOM, "sse_feed", at, rc);
            else
                bad += hk_bad(rc != 0, "sse_feed ok", at, rc), done_sse = 1;
            httpk_sse_free(&ps);
        }
        if (!done_hv) {
            memset(&h, 0, sizeof h);
            h.hdrs = hv;
            h.cap_hdrs = 8;
            httpk_resp_head(head, sizeof head - 1, &h);
            dyn_sb_init(&sb);
            hk_arm(at);
            rc = httpk_hdr_value(&h, head, "via", &sb);
            hk_fail_at = 0;
            if (hk_failed)
                bad += hk_bad(rc != HTTPK_HV_OOM, "hdr_value", at, rc);
            else
                bad += hk_bad(rc != HTTPK_HV_COMBINED, "hdr_value ok", at, rc), done_hv = 1;
            dyn_sb_free(&sb);
        }
        dyn_sb_free(&before);
        httpk_jar_free(&j);
        rc = 0;
    }
    CHECK(bad == 0 && done_set && done_hdr && done_sse && done_hv,
        "the Nth allocation failing gives each op's OOM result and leaves the jar as it was, then the op succeeds (bad %d, ops done %d%d%d%d after %lu)",
        bad, done_set, done_hdr, done_sse, done_hv, at);
}

static void prop(const char* name, unsigned long base, int kind)
{
    long it, n = (long)fk_iters(base);
    fk_rng_t r = fk_stream(name);
    hm_src_t s = { NULL, 0, &r };
    for (it = 0; it < n; it++) {
        const char* e = NULL;
        switch (kind) {
        case 0:
            e = hm_check_head(&s, NULL, 0);
            break;
        case 1:
            e = hm_check_chunk(&s, NULL, 0);
            break;
        case 2:
            e = hm_check_sse(&s, NULL, 0);
            break;
        case 3:
            e = hm_check_date(&s);
            break;
        default:
            hm_heavy = 1;
            e = it % 3 == 0 ? hm_check_head(&s, NULL, 0) : it % 3 == 1 ? hm_check_chunk(&s, NULL, 0) : hm_check_sse(&s, NULL, 0);
            hm_heavy = 0;
            break;
        }
        FK_CHECK(!e, "iter %ld: %s", it, e);
        if (e)
            break;
    }
}

int main(void)
{
    fk_init("test_httpk_core");
    if (fk_legacy()) {
        test_field();
        test_head();
        test_chunk();
        test_date();
        test_fixdate();
        test_retry();
        test_url_redirect();
        test_sse();
        test_cookie();
        test_req();
        test_alloc_failure();
        printf("test_httpk_core: %d checks, %d failed\n", checks, fails);
    }
    if (fk_only("hk-head"))
        prop("hk-head", 15000, 0);
    if (fk_only("hk-chunk"))
        prop("hk-chunk", 8000, 1);
    if (fk_only("hk-sse"))
        prop("hk-sse", 15000, 2);
    if (fk_only("hk-date"))
        prop("hk-date", 100000, 3);
    if (fk_only("hk-raw"))
        prop("hk-raw", 15000, 4);
    return fk_done("test_httpk_core") || fails;
}
