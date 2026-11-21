/*
 * test_cfgscan_core.c -- src/core/cfgscan (the dyna:config line cursor,
 * front-matter finder and .env/INI scanners) as a standalone C library.
 *
 * Oracles:
 *  - Front matter fences as Jekyll and Hugo define them: "---" YAML,
 *    "+++" TOML, ";;;" JSON; the block must be closed by the same fence on a
 *    line of its own; CRLF files behave like LF files.
 *  - .env records as the dotenv family reads them: blank and "#" lines are
 *    skipped, an unquoted value ends at "#" and is trimmed, a double-quoted
 *    value may contain \" and "#", a single-quoted value is literal.
 *  - POSIX environ: a name is non-empty and holds no "=" or NUL.
 *  - TOML v1.0.0 lexical grammar: basic strings take the escapes \b \t \n
 *    \f \r \" \\ \uXXXX \UXXXXXXXX (Unicode scalar values only); literal
 *    strings take none; a multi-line string drops the newline right after
 *    its opener, a line-ending backslash trims the following whitespace, and
 *    up to two quotes may sit before the closing delimiter, and a newline is
 *    LF or CRLF only (a bare CR is a control character, fix-DC campaign
 *    note F-DC2); integers are
 *    64-bit with 0x/0o/0b prefixes and underscores only between digits;
 *    floats need digits on both sides of the dot; dates and times follow
 *    RFC 3339 with T, t or space as the separator (spec examples).
 * Properties: every value range the scanner returns lies inside the input,
 * and the cursor always advances (checked over all 4^6 strings on a 4-letter
 * alphabet); the TOML lexer stays inside an unterminated heap copy of every
 * string up to length 4 on a 14-letter alphabet (ASan), and on error it
 * reports a position at most one column past the input.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <stdint.h>

#include "dyn-cfgscan.h"

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

static void fm(const char* src, const char* lang, const char* data, const char* body)
{
    size_t ds = 0, de = 0, bs = 0, n = strlen(src);
    const char* l = NULL;
    int found = dyn_frontmatter_find(src, n, &ds, &de, &bs, &l);
    if (!lang) {
        CHECK(!found, "no front matter in [%s]", src);
        return;
    }
    CHECK(found && !strcmp(l, lang) && de - ds == strlen(data) && !memcmp(src + ds, data, de - ds)
            && !strcmp(src + bs, body),
        "front matter [%s] -> %s data [%.*s] body [%s]", src, found ? l : "none", (int)(de - ds), src + ds,
        found ? src + bs : "");
}

static void test_frontmatter(void)
{
    fm("---\ntitle: x\n---\nbody\n", "yaml", "title: x", "body\n");
    fm("+++\ntitle = 1\n+++\nb", "toml", "title = 1", "b");
    fm(";;;\n{\"a\":1}\n;;;\n", "json", "{\"a\":1}", "");
    fm("---\r\na: 1\r\n---\r\nbody\r\n", "yaml", "a: 1", "body\r\n");
    fm("  ---  \nk: v\n ---\nrest", "yaml", "k: v", "rest");
    fm("---\n---\nonly body", "yaml", "", "only body");
    fm("---\na: 1\n", NULL, NULL, NULL);
    fm("----\na\n----\n", NULL, NULL, NULL);
    fm("----\na\n---\n", NULL, NULL, NULL);
    fm("---\na\n+++\n", NULL, NULL, NULL);
    fm("***\na\n***\n", NULL, NULL, NULL);
    fm("text\n---\na\n---\n", NULL, NULL, NULL);
    fm("", NULL, NULL, NULL);
    fm("---\na\n---", "yaml", "a", "");
}

static void test_lines(void)
{
    const char* s = "a\r\nbb\n\nccc";
    dyn_cur_t c;
    size_t b, e;
    c.s = s;
    c.n = strlen(s);
    c.i = 0;
    CHECK(dyn_next_line(&c, &b, &e) && e - b == 1 && s[b] == 'a', "CRLF line loses its CR");
    CHECK(dyn_next_line(&c, &b, &e) && e - b == 2, "LF line");
    CHECK(dyn_next_line(&c, &b, &e) && e == b, "an empty line is a line");
    CHECK(dyn_next_line(&c, &b, &e) && e - b == 3 && c.i == c.n, "a last line without a newline");
    CHECK(!dyn_next_line(&c, &b, &e), "then the cursor is exhausted");
    {
        const char* w = " \t\v\fx y\r\n";
        size_t tb = 0, te = strlen(w);
        dyn_trim(w, &tb, &te);
        CHECK(te - tb == 3 && w[tb] == 'x', "trim strips the C isspace set");
    }
    {
        const char* bom = "\xEF\xBB\xBFk=v";
        size_t n = strlen(bom);
        dyn_cfg_bom(&bom, &n);
        CHECK(n == 3 && *bom == 'k', "UTF-8 BOM skipped");
    }
}

static void val(const char* line, const char* want, char wantq)
{
    size_t i = 0, vb = 0, ve = 0, n = strlen(line);
    char q = 'x';
    dyn_env_read_value(line, n, &i, &vb, &ve, &q);
    CHECK(q == wantq && ve - vb == strlen(want) && !memcmp(line + vb, want, ve - vb) && (i == n || line[i] == '\n'),
        "value [%s] -> [%.*s] q=%c, want [%s] q=%c", line, (int)(ve - vb), line + vb, q ? q : '0', want,
        wantq ? wantq : '0');
}

static void test_env(void)
{
    val("plain value  # comment", "plain value", 0);
    val("  spaced\t", "spaced", 0);
    val("\"a \\\" # b\" # c", "a \\\" # b", '"');
    val("'lit \\' x", "lit \\", '\'');
    val("\"unterminated", "unterminated", '"');
    val("v\nNEXT=1", "v", 0);
    val("", "", 0);
    {
        const char* s = "\n\n  # c1\n\t# c2\r\n  KEY=1\n# end";
        size_t i = 0;
        CHECK(dyn_env_next_record(s, strlen(s), &i) && !strncmp(s + i, "KEY=1", 5), "comments and blanks skipped");
        i += 5;
        CHECK(!dyn_env_next_record(s, strlen(s), &i), "a trailing comment ends the file");
    }
    {
        char big[DYN_ENV_KEY_MAX + 2];
        memset(big, 'K', sizeof big);
        big[sizeof big - 1] = 0;
        CHECK(dyn_env_key_ok("PATH", 4) && !dyn_env_key_ok("", 0) && !dyn_env_key_ok("A=B", 3)
                && !dyn_env_key_ok("A\0B", 3),
            "POSIX environ names");
        CHECK(!dyn_env_key_ok(big, DYN_ENV_KEY_MAX + 1), "a name over the bound is refused");
        big[DYN_ENV_KEY_MAX] = 0;
        CHECK(dyn_env_key_ok(big, DYN_ENV_KEY_MAX), "a name at the bound is accepted");
        CHECK(!dyn_env_key_ok(big, DYN_ENV_KEY_MAX - 1), "a length that disagrees with strlen is refused");
    }
    CHECK(dyn_env_needs_quotes("", 0) && dyn_env_needs_quotes("a b", 3) && dyn_env_needs_quotes("a#", 2)
            && dyn_env_needs_quotes("'", 1) && !dyn_env_needs_quotes("a=b;c", 5),
        ".env quoting");
    CHECK(dyn_ini_needs_quotes("", 0) && dyn_ini_needs_quotes(" a", 2) && dyn_ini_needs_quotes("a ", 2)
            && dyn_ini_needs_quotes("a;b", 3) && dyn_ini_needs_quotes("a=b", 3) && dyn_ini_needs_quotes("a\\b", 3)
            && !dyn_ini_needs_quotes("a b", 3),
        "INI quoting");
}

static void test_scanner_bounds(void)
{
    static const char A[4] = { '"', '#', '\n', 'x' };
    char s[7];
    int k, bad = 0;
    for (k = 0; k < 4096; k++) {
        size_t i = 0, vb, ve, j;
        char q;
        int m = k;
        for (j = 0; j < 6; j++, m >>= 2)
            s[j] = A[m & 3];
        s[6] = 0;
        dyn_env_read_value(s, 6, &i, &vb, &ve, &q);
        if (!(vb <= ve && ve <= 6 && i <= 6))
            bad++;
        i = 0;
        while (dyn_env_next_record(s, 6, &i)) {
            size_t before = i;
            dyn_env_read_value(s, 6, &i, &vb, &ve, &q);
            if (i < 6)
                i++;
            if (i <= before)
                bad++;
            if (bad)
                break;
        }
    }
    CHECK(bad == 0, "scanner ranges stay in bounds and the cursor advances (%d bad)", bad);
}

static double tml_strtod(const char* buf, const char** end)
{
    char* e;
    double d = strtod(buf, &e);
    if (end)
        *end = e;
    return d;
}

static void tstr(const char* src, const char* want, size_t wn, size_t consumed)
{
    dyn_tml_lex_t t;
    dyn_tml_sb_t sb;
    int r;
    dyn_tml_lex_init(&t, src, strlen(src));
    dyn_tml_sb_init(&sb);
    r = dyn_tml_string(&t, &sb);
    CHECK(r == 0 && sb.n == wn && (wn == 0 || memcmp(sb.p, want, wn) == 0) && t.i == consumed,
        "string %s: r=%d n=%zu i=%zu", src, r, sb.n, t.i);
    dyn_tml_sb_free(&sb);
}

static void tstr_err(const char* src, int code, int line, int col)
{
    dyn_tml_lex_t t;
    dyn_tml_sb_t sb;
    int r;
    dyn_tml_lex_init(&t, src, strlen(src));
    dyn_tml_sb_init(&sb);
    r = dyn_tml_string(&t, &sb);
    CHECK(r < 0 && t.ecode == code && t.eline == line && t.ecol == col,
        "string error %s: r=%d code=%d (%s) at %d:%d", src, r, t.ecode, dyn_tml_errstr(t.ecode), t.eline, t.ecol);
    dyn_tml_sb_free(&sb);
}

static int tscal(const char* src, dyn_tml_scalar_t* v, dyn_tml_lex_t* t)
{
    dyn_tml_lex_init(t, src, strlen(src));
    memset(v, 0, sizeof *v);
    return dyn_tml_scalar(t, v, tml_strtod);
}

static void tint(const char* src, int64_t want)
{
    dyn_tml_lex_t t;
    dyn_tml_scalar_t v;
    int r = tscal(src, &v, &t);
    CHECK(r == 0 && v.kind == DYN_TML_V_INT && v.i == want && t.i == strlen(src),
        "int %s: r=%d kind=%d i=%lld", src, r, v.kind, (long long)v.i);
}

static void tflt(const char* src, double want)
{
    dyn_tml_lex_t t;
    dyn_tml_scalar_t v;
    int r = tscal(src, &v, &t);
    int same = isnan(want) ? isnan(v.d) : (v.d == want && signbit(v.d) == signbit(want));
    CHECK(r == 0 && v.kind == DYN_TML_V_FLOAT && same && t.i == strlen(src),
        "float %s: r=%d kind=%d d=%g", src, r, v.kind, v.d);
}

static void tdt(const char* src, size_t len)
{
    dyn_tml_lex_t t;
    dyn_tml_scalar_t v;
    int r = tscal(src, &v, &t);
    CHECK(r == 0 && v.kind == DYN_TML_V_DATETIME && v.off == 0 && v.len == len && t.i == len && t.col == 1 + (int)len,
        "datetime %s: r=%d kind=%d len=%zu", src, r, v.kind, v.len);
}

static void tbad(const char* src, int code, int col)
{
    dyn_tml_lex_t t;
    dyn_tml_scalar_t v;
    int r = tscal(src, &v, &t);
    CHECK(r < 0 && t.ecode == code && t.eline == 1 && t.ecol == col,
        "scalar error %s: r=%d code=%d (%s) col %d", src, r, t.ecode, dyn_tml_errstr(t.ecode), t.ecol);
}

static void test_toml_strings(void)
{
    tstr("\"I'm a string. \\\"You can quote me\\\". Name\\tJos\\u00E9\\nLocation\\tSF.\"",
        "I'm a string. \"You can quote me\". Name\tJos\xC3\xA9\nLocation\tSF.", 57, 68);
    tstr("\"\\b\\f\\r\\\\\"", "\b\f\r\\", 4, 10);
    tstr("\"\\u007F\\u0080\\u07FF\\u0800\\uFFFF\\U00010000\\U0010FFFF\"",
        "\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF", 19, 52);
    tstr("'C:\\Users\\nodejs\\templates'", "C:\\Users\\nodejs\\templates", 25, 27);
    tstr("\"\"", "", 0, 2);
    tstr("\"\"\"\nRoses are red\nViolets are blue\"\"\"", "Roses are red\nViolets are blue", 30, 37);
    tstr("\"\"\"\r\nA\r\nB\"\"\"", "A\nB", 3, 12);
    tstr("\"\"\"\nThe quick brown \\\n\n\n  fox jumps over \\\n    the lazy dog.\"\"\"",
        "The quick brown fox jumps over the lazy dog.", 44, 63);
    tstr("\"\"\"a \\  \t\n  b\"\"\"", "a b", 3, 16);
    tstr("\"\"\"Here are two quotation marks: \"\". Simple enough.\"\"\"",
        "Here are two quotation marks: \"\". Simple enough.", 48, 54);
    tstr("\"\"\"\"This,\" she said, \"is just a pointless statement.\"\"\"\"",
        "\"This,\" she said, \"is just a pointless statement.\"", 50, 56);
    tstr("'''\nThe first newline is\ntrimmed in raw strings.\n   All other whitespace\n   is preserved.\n'''",
        "The first newline is\ntrimmed in raw strings.\n   All other whitespace\n   is preserved.\n", 86, 93);
    tstr("''''That,' she said, 'is still pointless.''''",
        "'That,' she said, 'is still pointless.'", 39, 45);
    tstr("'''x'''''", "x''", 3, 9);
    tstr("\"a\" = 1", "a", 1, 3);

    tstr_err("\"abc", DYN_TML_E_UNTERM, 1, 5);
    tstr_err("\"ab\ncd\"", DYN_TML_E_UNTERM, 1, 4);
    tstr_err("'ab\n'", DYN_TML_E_UNTERM_LIT, 1, 4);
    tstr_err("\"a\x01\"", DYN_TML_E_CTRL, 1, 3);
    tstr_err("\"\"\"x\ry\"\"\"", DYN_TML_E_CTRL_ML, 1, 5);
    tstr_err("'''x\ry'''", DYN_TML_E_CTRL_MLL, 1, 5);
    tstr_err("\"\"\"x\r\"\"\"", DYN_TML_E_CTRL_ML, 1, 5);
    tstr_err("\"\"\"a\\\n\r b\"\"\"", DYN_TML_E_CTRL_ML, 2, 1);
    tstr("\"\"\"a\\\r\n\r\n b\"\"\"", "ab", 2, 14);
    tstr_err("\"a\x7F\"", DYN_TML_E_CTRL, 1, 3);
    tstr_err("'a\x1F'", DYN_TML_E_CTRL_LIT, 1, 3);
    tstr_err("\"\\x41\"", DYN_TML_E_BAD_ESCAPE, 1, 4);
    tstr_err("\"\\u12G4\"", DYN_TML_E_BAD_U, 1, 6);
    tstr_err("\"\\uD800\"", DYN_TML_E_NOT_SCALAR, 1, 8);
    tstr_err("\"\\U00110000\"", DYN_TML_E_NOT_SCALAR, 1, 12);
    tstr_err("\"\\uDFFF\"", DYN_TML_E_NOT_SCALAR, 1, 8);
    tstr("\"\\uD7FF\\uE000\"", "\xED\x9F\xBF\xEE\x80\x80", 6, 14);
    tstr_err("\"\\", DYN_TML_E_BAD_ESCAPE, 1, 4);
    tstr_err("\"\"\"abc", DYN_TML_E_UNTERM_ML, 1, 7);
    tstr_err("\"\"\"a\n\x02\"\"\"", DYN_TML_E_CTRL_ML, 2, 1);
    tstr_err("\"\"\"a\"\"\"\"\"\"", DYN_TML_E_QUOTES, 1, 5);
    tstr_err("\"\"\"a\\ b\"\"\"", DYN_TML_E_ML_ESCAPE, 1, 7);
    tstr_err("'''abc", DYN_TML_E_UNTERM_MLL, 1, 7);
    tstr_err("'''a\x7F'''", DYN_TML_E_CTRL_MLL, 1, 5);
    tstr_err("'''a''''''", DYN_TML_E_APOS, 1, 5);
    {
        dyn_tml_lex_t t;
        dyn_tml_sb_t sb;
        dyn_tml_scalar_t v;
        char buf[128];
        dyn_tml_lex_init(&t, "\"\\q\"", 4);
        dyn_tml_sb_init(&sb);
        dyn_tml_string(&t, &sb);
        dyn_tml_format_error(&t, buf, sizeof buf);
        CHECK(strcmp(buf, "line 1 col 4: invalid escape \\q") == 0, "formatted escape error: %s", buf);
        dyn_tml_sb_free(&sb);
        dyn_tml_lex_init(&t, "1979-13-01", 10);
        dyn_tml_scalar(&t, &v, tml_strtod);
        dyn_tml_format_error(&t, buf, sizeof buf);
        CHECK(strcmp(buf, "line 1 col 1: invalid date-time") == 0, "formatted date error: %s", buf);
        dyn_tml_lex_init(&t, "'ok'", 4);
        dyn_tml_sb_init(&sb);
        sb.oom = 1;
        CHECK(dyn_tml_string(&t, &sb) < 0 && t.ecode == DYN_TML_E_OOM, "a buffer that failed to grow is reported as out of memory");
        dyn_tml_sb_free(&sb);
        CHECK(strcmp(dyn_tml_errstr(DYN_TML_E_COUNT), "") == 0 && strcmp(dyn_tml_errstr(DYN_TML_E_NONE), "") == 0, "errstr out of range is empty");
    }
    {
        int code, empty = 0;
        for (code = DYN_TML_E_NONE + 1; code < DYN_TML_E_COUNT; code++)
            if (!dyn_tml_errstr(code)[0])
                empty++;
        CHECK(empty == 0, "every TOML error code has text (%d empty)", empty);
    }
}

static void tkey(const char* src, size_t nseg, const char* const* segs, size_t consumed)
{
    dyn_tml_lex_t t;
    dyn_tml_key_t k;
    size_t j;
    int ok;
    dyn_tml_lex_init(&t, src, strlen(src));
    dyn_tml_key_init(&k);
    ok = dyn_tml_key_parse(&t, &k) == 0 && k.nseg == nseg && t.i == consumed;
    for (j = 0; ok && j < nseg; j++)
        ok = k.seg[j].len == strlen(segs[j]) && memcmp(k.p + k.seg[j].off, segs[j], k.seg[j].len) == 0;
    CHECK(ok, "key %s: nseg=%zu i=%zu", src, k.nseg, t.i);
    dyn_tml_key_free(&k);
}

static void test_toml_keys(void)
{
    static const char* const k1[] = { "bare_key-1" };
    static const char* const k2[] = { "physical", "color" };
    static const char* const k3[] = { "site", "google.com" };
    static const char* const k4[] = { "fruit", "flavor", "", "x y" };
    static const char* const k5[] = { "1234" };
    static const char* const k6[] = { "3", "14159" };
    char big[600];
    const char* segs[40];
    size_t j;
    tkey("bare_key-1 = 1", 1, k1, 11);
    tkey("physical.color = \"orange\"", 2, k2, 15);
    tkey("site.\"google.com\" = true", 2, k3, 18);
    tkey("  fruit . flavor . \"\" . 'x y'=", 4, k4, 29);
    tkey("1234 = 1", 1, k5, 5);
    tkey("3.14159 = \"pi\"", 2, k6, 8);
    big[0] = 0;
    for (j = 0; j < 40; j++) {
        segs[j] = "abcdefghijklm";
        strcat(big, "abcdefghijklm");
        if (j + 1 < 40)
            strcat(big, ".");
    }
    tkey(big, 40, segs, strlen(big));
    {
        dyn_tml_lex_t t;
        dyn_tml_key_t k;
        dyn_tml_lex_init(&t, "a.=1", 4);
        dyn_tml_key_init(&k);
        CHECK(dyn_tml_key_parse(&t, &k) < 0 && t.ecode == DYN_TML_E_KEY && t.ecol == 3, "dangling dot is refused at col 3 (%d)", t.ecol);
        dyn_tml_key_free(&k);
        dyn_tml_lex_init(&t, "\"a\nb\" = 1", 9);
        dyn_tml_key_init(&k);
        CHECK(dyn_tml_key_parse(&t, &k) < 0 && t.ecode == DYN_TML_E_UNTERM, "newline in a quoted key is refused");
        dyn_tml_key_free(&k);
    }
}

static void test_toml_scalars(void)
{
    dyn_tml_lex_t t;
    dyn_tml_scalar_t v;
    tint("+99", 99);
    tint("42", 42);
    tint("0", 0);
    tint("-17", -17);
    tint("1_000", 1000);
    tint("5_349_221", 5349221);
    tint("0xDEADBEEF", 0xDEADBEEF);
    tint("0xdead_beef", 0xDEADBEEF);
    tint("0o01234567", 01234567);
    tint("0o755", 0755);
    tint("0b11010110", 214);
    tint("9223372036854775807", INT64_MAX);
    tint("-9223372036854775808", INT64_MIN);
    tint("0x7FFFFFFFFFFFFFFF", INT64_MAX);
    tflt("+1.0", 1.0);
    tflt("3.1415", 3.1415);
    tflt("-0.01", -0.01);
    tflt("5e+22", 5e22);
    tflt("1e06", 1e6);
    tflt("-2E-2", -2e-2);
    tflt("6.626e-34", 6.626e-34);
    tflt("224_617.445_991_228", 224617.445991228);
    tflt("-0.0", -0.0);
    tflt("+0.0", 0.0);
    tflt("inf", HUGE_VAL);
    tflt("+inf", HUGE_VAL);
    tflt("-inf", -HUGE_VAL);
    tflt("nan", (double)NAN);
    tflt("+nan", (double)NAN);
    tflt("-nan", (double)NAN);
    CHECK(tscal("true", &v, &t) == 0 && v.kind == DYN_TML_V_BOOL && v.b == 1 && t.i == 4, "true");
    CHECK(tscal("false]", &v, &t) == 0 && v.kind == DYN_TML_V_BOOL && v.b == 0 && t.i == 5, "false");
    tdt("1979-05-27T07:32:00Z", 20);
    tdt("1979-05-27T00:32:00-07:00", 25);
    tdt("1979-05-27T00:32:00.999999-07:00", 32);
    tdt("1979-05-27 07:32:00Z", 20);
    tdt("1979-05-27t07:32:00z", 20);
    tdt("1979-05-27T07:32:00", 19);
    tdt("1979-05-27T00:32:00.999999", 26);
    tdt("1979-05-27", 10);
    tdt("07:32:00", 8);
    tdt("00:32:00.999999", 15);
    tdt("23:59:60", 8);
    CHECK(tscal("1979-05-27 # c", &v, &t) == 0 && v.kind == DYN_TML_V_DATETIME && v.len == 10 && t.i == 10, "a space after a local date that is not followed by a time ends the date (TOML 1.0 local date + comment)");
    CHECK(tscal("1979-05-27 ", &v, &t) == 0 && v.len == 10, "a local date with a trailing space");
    CHECK(tscal("1979-05-27 1", &v, &t) == 0 && v.len == 10, "a space plus a lone digit is not a time");
    CHECK(tscal("1979-05-27 123", &v, &t) == 0 && v.len == 10, "a space plus digits without a colon is not a time");
    CHECK(tscal("1979-05-27 ab:", &v, &t) == 0 && v.len == 10, "a space plus non-digits and a colon is not a time");
    CHECK(tscal("1979-05-27 07:3", &v, &t) < 0 && t.ecode == DYN_TML_E_DATETIME, "a space plus HH: commits to a time, and a cut time is refused");

    tbad("+", DYN_TML_E_VALUE, 2);
    tbad("=", DYN_TML_E_VALUE, 1);
    tbad("1x", DYN_TML_E_INT, 3);
    tbad("12g", DYN_TML_E_NUM, 3);
    tbad("1__2", DYN_TML_E_UNDERSCORE, 5);
    tbad("_1", DYN_TML_E_UNDERSCORE, 3);
    tbad("1_", DYN_TML_E_UNDERSCORE, 3);
    tbad("0x_1", DYN_TML_E_UNDERSCORE, 5);
    tbad(".5", DYN_TML_E_DOT_BEFORE, 3);
    tbad("1.", DYN_TML_E_DOT_AFTER, 3);
    tbad("1.e5", DYN_TML_E_DOT_AFTER, 5);
    tbad("1e", DYN_TML_E_EXP, 3);
    tbad("1e+", DYN_TML_E_EXP, 4);
    tbad("1.2.3", DYN_TML_E_FLOAT, 6);
    tbad("1ee5", DYN_TML_E_EXP, 5);
    tbad("1e5e5", DYN_TML_E_FLOAT, 6);
    tbad("12a", DYN_TML_E_BASE, 4);
    tbad("1o", DYN_TML_E_INT, 3);
    tbad("0o8", DYN_TML_E_BASE, 4);
    tbad("0b102", DYN_TML_E_BASE, 6);
    tbad("0x", DYN_TML_E_NUM, 3);
    tbad("01", DYN_TML_E_LEADING_ZERO, 3);
    tbad("+01.5", DYN_TML_E_LEADING_ZERO, 6);
    tbad("9223372036854775808", DYN_TML_E_INT_RANGE, 20);
    tbad("-9223372036854775809", DYN_TML_E_INT_RANGE, 21);
    tbad("0x1_0000_0000_0000_0000", DYN_TML_E_INT_RANGE, 24);
    tbad("99999999999999999999999", DYN_TML_E_INT_RANGE, 24);
    tbad("1979-13-27", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-32", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-00", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T24:00:00", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:60:00", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:32:61", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:32", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:32:00.", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:32:00+24:00", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:32:00+07:60", DYN_TML_E_DATETIME, 1);
    tbad("1979-05-27T07:32:00+0700", DYN_TML_E_DATETIME, 1);
    tbad("12:3:00", DYN_TML_E_DATETIME, 1);
    {
        char longnum[140];
        memset(longnum, '1', 130);
        longnum[130] = 0;
        tbad(longnum, DYN_TML_E_NUM_LONG, 131);
        longnum[128] = 0;
        tbad(longnum, DYN_TML_E_NUM_LONG, 129);
        memset(longnum, '1', 127);
        longnum[127] = 0;
        tbad(longnum, DYN_TML_E_INT_RANGE, 128);
    }
}

static void test_toml_bounds(void)
{
    static const char alph[] = "\"'\\u01e.-:T\n_x ";
    enum { NA = sizeof alph - 1 };
    int len, k, bad = 0;
    long total = 0;
    for (len = 1; len <= 4; len++) {
        long combos = 1, c;
        for (k = 0; k < len; k++)
            combos *= NA;
        for (c = 0; c < combos; c++) {
            long q = c;
            char* heap = (char*)malloc((size_t)len);
            dyn_tml_lex_t t;
            dyn_tml_sb_t sb;
            dyn_tml_scalar_t v;
            dyn_tml_key_t key;
            for (k = 0; k < len; k++) {
                heap[k] = alph[q % NA];
                q /= NA;
            }
            dyn_tml_lex_init(&t, heap, (size_t)len);
            if (heap[0] == '"' || heap[0] == '\'') {
                dyn_tml_sb_init(&sb);
                if (dyn_tml_string(&t, &sb) < 0 && t.ecol > len + 2)
                    bad++;
                dyn_tml_sb_free(&sb);
            } else if (dyn_tml_scalar(&t, &v, tml_strtod) == 0) {
                if (v.kind == DYN_TML_V_DATETIME && v.off + v.len > (size_t)len)
                    bad++;
            } else if (t.ecol > len + 1)
                bad++;
            if (t.i > (size_t)len)
                bad++;
            dyn_tml_lex_init(&t, heap, (size_t)len);
            dyn_tml_key_init(&key);
            if (dyn_tml_key_parse(&t, &key) == 0 && (t.i > (size_t)len || key.n > (size_t)len))
                bad++;
            dyn_tml_key_free(&key);
            free(heap);
            total++;
        }
    }
    CHECK(bad == 0 && total > 38000, "TOML lexer stays in bounds over %ld inputs (%d bad)", total, bad);
}

int main(void)
{
    test_frontmatter();
    test_lines();
    test_env();
    test_scanner_bounds();
    test_toml_strings();
    test_toml_keys();
    test_toml_scalars();
    test_toml_bounds();
    printf("test_cfgscan_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
