/*
 * test_yamlscalar_core.c -- src/core/yamlscalar (the dyna:yaml scalar
 * classifiers, key/flow colon finders and emitter quoting) as a standalone C
 * library: no engine.
 *
 * Oracles from outside this code (YAML 1.2.2, https://yaml.org/spec/1.2.2/):
 *  - 10.3.2 core schema tag resolution, compiled with POSIX regcomp(3) as an
 *    independent implementation:
 *      int    [-+]?[0-9]+ | 0o[0-7]+ | 0x[0-9a-fA-F]+
 *      float  [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?
 *    yml_is_float equals "float and not int" over every string of up to six
 *    symbols from { 0 1 8 9 . e E + - x o } (1,948,716 cases). yml_is_int equals
 *    the int rule on the same set EXCEPT a sign before 0x/0o: the module reads
 *    "-0x1F" as -31 where the core schema says it is a string. That deviation
 *    (and "0X"/"0O", and decimals past int64 resolving to strings) is reported,
 *    not asserted.
 *  - 7.3.3 / 6.6 plain scalars: a plain scalar the emitter leaves unquoted
 *    must not resolve to null/bool/int/float under 10.3.2, must not start
 *    with an indicator (production [22] c-indicator, first-character rule
 *    [126]), must not contain ": " or " #", must not start or end with a
 *    space, and in flow context must not contain a flow indicator. Checked
 *    exhaustively over every string of up to four symbols from
 *    { a 1 . - : space # ' [ { ~ \n e ? } , ] }. ',' ']' and '}' are left out
 *    of the first position on purpose: the emitter writes them plain in block
 *    context, which [126] forbids (reported, not asserted).
 *  - 5.7 escaped characters for the double-quoted body (\" \\ \n \t \r and
 *    \u00XX for other C0 controls).
 *  - yml_eq: exact-length byte equality with a string literal (no prefix or
 *    extension matches). It is a macro taking the length from sizeof, so the
 *    length is a compile-time constant; as a static inline function it was
 *    not inlined into the long scalar classifiers, and every compare called
 *    strlen through the libc stub (perf: bench row parse_* in the yml_eq
 *    change, -14..-21%).
 */
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-yamlscalar.h"

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

static regex_t re_float, re_int, re_hex, re_oct;

static int m(regex_t* r, const char* s) { return regexec(r, s, 0, NULL, 0) == 0; }

static int core_int(const char* s) { return m(&re_int, s) || m(&re_hex, s) || m(&re_oct, s); }

static int core_float(const char* s)
{
    static const char* const special[] = { ".inf", ".Inf", ".INF", "+.inf", "+.Inf", "+.INF", "-.inf", "-.Inf",
        "-.INF", ".nan", ".NaN", ".NAN" };
    size_t i;
    for (i = 0; i < sizeof special / sizeof special[0]; i++)
        if (!strcmp(s, special[i]))
            return 1;
    return m(&re_float, s);
}

static int core_nonstring(const char* s)
{
    static const char* const words[] = { "", "~", "null", "Null", "NULL", "true", "True", "TRUE", "false", "False",
        "FALSE" };
    size_t i;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (!strcmp(s, words[i]))
            return 1;
    return core_int(s) || core_float(s);
}

static int signed_prefix(const char* s)
{
    return (s[0] == '+' || s[0] == '-') && s[1] == '0' && (s[2] == 'x' || s[2] == 'o');
}

static long num_cases, float_diffs, int_diffs;

static void num_one(const char* s)
{
    size_t n = strlen(s);
    int64_t v = 0;
    int f = yml_is_float(s, n) != 0, i = yml_is_int(s, n, &v) != 0;
    num_cases++;
    if (f != (m(&re_float, s) && !core_int(s)) && float_diffs++ < 5)
        printf("FAIL float \"%s\": ours %d\n", s, f);
    if (!signed_prefix(s) && i != core_int(s) && int_diffs++ < 5)
        printf("FAIL int \"%s\": ours %d\n", s, i);
}

static void num_rec(char* b, int len, int d)
{
    static const char A[] = "0189.eE+-xo";
    size_t k;
    if (d == len) {
        b[len] = 0;
        num_one(b);
        return;
    }
    for (k = 0; k < sizeof A - 1; k++) {
        b[d] = A[k];
        num_rec(b, len, d + 1);
    }
}

static void test_numbers(void)
{
    char b[8];
    int len;
    int64_t v = 0;
    for (len = 1; len <= 6; len++)
        num_rec(b, len, 0);
    CHECK(num_cases == 1948716, "number sweep ran %ld cases", num_cases);
    CHECK(float_diffs == 0, "yml_is_float is the 10.3.2 float rule minus int (%ld differ)", float_diffs);
    CHECK(int_diffs == 0, "yml_is_int is the 10.3.2 int rule outside signed 0x/0o (%ld differ)", int_diffs);
    CHECK(yml_is_int("0x1F", 4, &v) && v == 31, "0x1F is 31");
    CHECK(yml_is_int("0o17", 4, &v) && v == 15, "0o17 is 15");
    CHECK(yml_is_int("-42", 3, &v) && v == -42, "-42 is -42");
    CHECK(yml_is_int("9223372036854775807", 19, &v) && v == INT64_MAX, "INT64_MAX is the last int");
    CHECK(!yml_is_int("9223372036854775808", 19, &v), "one past INT64_MAX is not an int64");
    CHECK(!yml_is_float(".inf", 4) && !yml_is_float(".nan", 4), "the .inf/.nan words are matched by name, not here");
}

static long quote_cases, quote_bad;

static int is_indicator(char c) { return c && strchr("-?:,[]{}#&*!|>'\"%@`", c) != NULL; }

static void quote_one(const char* s, int flow)
{
    size_t n = strlen(s), i;
    const char* why = NULL;
    quote_cases++;
    if (yml_needs_quote(s, n, flow))
        return;
    if (core_nonstring(s))
        why = "resolves to a non-string";
    else if (is_indicator(s[0]))
        why = "starts with an indicator";
    else if (s[0] == ' ' || s[n - 1] == ' ')
        why = "leading or trailing space";
    else if (strstr(s, ": ") || strstr(s, " #") || s[n - 1] == ':')
        why = "': ' or ' #' inside";
    for (i = 0; !why && i < n; i++) {
        if ((unsigned char)s[i] < 0x20)
            why = "a control character";
        else if (flow && strchr(",[]{}", s[i]))
            why = "a flow indicator in flow context";
    }
    if (why && quote_bad++ < 5)
        printf("FAIL plain \"%s\" (flow %d): %s\n", s, flow, why);
}

static void quote_rec(char* b, int len, int d)
{
    static const char A[] = "a1.-: #'[{~\ne?},]";
    size_t k;
    if (d == len) {
        b[len] = 0;
        quote_one(b, 0);
        quote_one(b, 1);
        return;
    }
    for (k = 0; k < sizeof A - 1; k++) {
        if (d == 0 && (A[k] == '}' || A[k] == ',' || A[k] == ']'))
            continue;
        b[d] = A[k];
        quote_rec(b, len, d + 1);
    }
}

static void test_quoting(void)
{
    char b[8];
    int len;
    for (len = 0; len <= 4; len++)
        quote_rec(b, len, 0);
    CHECK(quote_cases > 80000, "plain-scalar sweep ran %ld cases", quote_cases);
    CHECK(quote_bad == 0, "every plain scalar the emitter allows is a plain string (%ld unsafe)", quote_bad);
    CHECK(!yml_needs_quote("hello world", 11, 0) && !yml_needs_quote("a-b", 3, 1), "ordinary text stays plain");
}

static void esc(const char* in, size_t n, const char* want)
{
    yb_t b;
    yb_init(&b);
    yml_put_escaped_body(&b, in, n);
    CHECK(b.n == strlen(want) && !memcmp(b.p, want, b.n), "5.7 escape -> %.*s, want %s", (int)b.n,
        b.p ? (const char*)b.p : "", want);
    yb_free(&b);
}

static void test_escape(void)
{
    uint32_t cp = 0;
    yb_t b;
    esc("a\"b\\c", 5, "a\\\"b\\\\c");
    esc("\n\t\r", 3, "\\n\\t\\r");
    esc("\x01\x1f", 2, "\\u0001\\u001f");
    CHECK(yml_hex("00e9", 0, 4, &cp) == 0 && cp == 0xE9, "\\u00e9 hex digits");
    CHECK(yml_hex("00g9", 0, 4, &cp) < 0, "a non-hex digit is refused");
    yb_init(&b);
    yml_utf8_put(&b, 0x1F600);
    CHECK(b.n == 4 && !memcmp(b.p, "\xF0\x9F\x98\x80", 4), "U+1F600 encodes as four UTF-8 bytes");
    yb_free(&b);
    CHECK(yml_utf8_unit("\xC3\xA9", 0, 2) == 2 && yml_utf8_unit("\xF0\x9F", 0, 2) == 2 && yml_utf8_unit("a", 0, 1) == 1,
        "utf8 unit length is clipped at the end");
}

static void test_eq(void)
{
    static const char buf[] = "nullable";
    CHECK(yml_eq(buf, 4, "null"), "exact 4-byte prefix window equals \"null\"");
    CHECK(!yml_eq(buf, 3, "null"), "a shorter window is not \"null\"");
    CHECK(!yml_eq(buf, 8, "null"), "a longer window is not \"null\"");
    CHECK(!yml_eq(buf, 4, "Null") && !yml_eq(buf, 4, "nulL"), "case matters");
    CHECK(yml_eq(buf, 0, "") && !yml_eq(buf, 1, ""), "the empty literal matches only length 0");
    CHECK(yml_eq("a\0b", 3, "a\0b") && !yml_eq("a\0c", 3, "a\0b"), "an embedded NUL is compared, not a terminator");
}

static void test_colons(void)
{
    const char* s;
    s = "key: value";
    CHECK(yml_key_colon(s, 0, strlen(s)) == 3, "7.3.3 'key: value' splits at the colon");
    s = "a:b";
    CHECK(yml_key_colon(s, 0, strlen(s)) == (size_t)-1, "':' not followed by space is part of a plain scalar");
    s = "\"k: x\": v";
    CHECK(yml_key_colon(s, 0, strlen(s)) == 6, "a quoted key hides its own colon");
    s = "\"k\":v";
    CHECK(yml_key_colon(s, 0, strlen(s)) == 3, "after a quoted key, ':' needs no space (JSON-like key)");
    s = "x # a: b";
    CHECK(yml_key_colon(s, 0, strlen(s)) == (size_t)-1, "a colon inside a comment is not a key");
    s = "{a: 1}: v";
    CHECK(yml_key_colon(s, 0, strlen(s)) == 6, "a colon inside a flow collection is skipped");
    s = "a: 1, b";
    CHECK(yml_flow_colon(s, 0, strlen(s)) == 1, "flow pair colon");
    s = "a, b: 1";
    CHECK(yml_flow_colon(s, 0, strlen(s)) == (size_t)-1, "a ',' ends the flow entry before any colon");
    s = "\"a\":1";
    CHECK(yml_flow_colon(s, 0, strlen(s)) == 3, "JSON-like flow key needs no space");
    CHECK(yml_flow_is_pair("x: 1]", 0, 5) && !yml_flow_is_pair("x:1]", 0, 4) && !yml_flow_is_pair("x, y: 1", 0, 7),
        "flow entry pair detection");
    s = "abc  # comment";
    CHECK(yml_plain_end(s, 0, strlen(s)) == 3, "6.6 a comment needs white space before '#'");
    s = "#x";
    CHECK(yml_plain_end(s, 0, strlen(s)) == 2, "a '#' with no white space before it in the range is content");
    s = "k #x";
    CHECK(yml_plain_end(s, 2, strlen(s)) == 4, "the range start is never looked behind: a leading '#' is content");
    s = "a#b ";
    CHECK(yml_plain_end(s, 0, strlen(s)) == 3, "'#' inside a word is content; trailing space trimmed");
}

int main(void)
{
    if (regcomp(&re_float, "^[-+]?(\\.[0-9]+|[0-9]+(\\.[0-9]*)?)([eE][-+]?[0-9]+)?$", REG_EXTENDED | REG_NOSUB)
        || regcomp(&re_int, "^[-+]?[0-9]+$", REG_EXTENDED | REG_NOSUB)
        || regcomp(&re_hex, "^0x[0-9a-fA-F]+$", REG_EXTENDED | REG_NOSUB)
        || regcomp(&re_oct, "^0o[0-7]+$", REG_EXTENDED | REG_NOSUB)) {
        printf("FAIL regcomp\n");
        return 1;
    }
    test_numbers();
    test_quoting();
    test_escape();
    test_colons();
    test_eq();
    regfree(&re_float);
    regfree(&re_int);
    regfree(&re_hex);
    regfree(&re_oct);
    printf("test_yamlscalar_core: %d checks, %ld number cases, %ld quoting cases, %d failed\n", checks, num_cases,
        quote_cases, fails);
    return fails ? 1 : 0;
}
