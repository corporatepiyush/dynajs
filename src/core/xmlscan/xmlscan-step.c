#include "xmlscan.h"

#define XML_SIMD_MIN 64

const uint8_t XML_NAME[256] = {
    ['A'] = 3,
    ['B'] = 3,
    ['C'] = 3,
    ['D'] = 3,
    ['E'] = 3,
    ['F'] = 3,
    ['G'] = 3,
    ['H'] = 3,
    ['I'] = 3,
    ['J'] = 3,
    ['K'] = 3,
    ['L'] = 3,
    ['M'] = 3,
    ['N'] = 3,
    ['O'] = 3,
    ['P'] = 3,
    ['Q'] = 3,
    ['R'] = 3,
    ['S'] = 3,
    ['T'] = 3,
    ['U'] = 3,
    ['V'] = 3,
    ['W'] = 3,
    ['X'] = 3,
    ['Y'] = 3,
    ['Z'] = 3,
    ['a'] = 3,
    ['b'] = 3,
    ['c'] = 3,
    ['d'] = 3,
    ['e'] = 3,
    ['f'] = 3,
    ['g'] = 3,
    ['h'] = 3,
    ['i'] = 3,
    ['j'] = 3,
    ['k'] = 3,
    ['l'] = 3,
    ['m'] = 3,
    ['n'] = 3,
    ['o'] = 3,
    ['p'] = 3,
    ['q'] = 3,
    ['r'] = 3,
    ['s'] = 3,
    ['t'] = 3,
    ['u'] = 3,
    ['v'] = 3,
    ['w'] = 3,
    ['x'] = 3,
    ['y'] = 3,
    ['z'] = 3,
    ['_'] = 3,
    [':'] = 3,
    ['0'] = 2,
    ['1'] = 2,
    ['2'] = 2,
    ['3'] = 2,
    ['4'] = 2,
    ['5'] = 2,
    ['6'] = 2,
    ['7'] = 2,
    ['8'] = 2,
    ['9'] = 2,
    ['-'] = 2,
    ['.'] = 2,
};

int xml_step(xml_scan_t* s, xml_ev_t* ev)
{
    const char* p = (const char*)s->buf.p;
    size_t n = s->buf.n, i = s->pos;
    int r;

    if (i >= n)
        return s->final ? XS_END : XS_NEED;
    if (!s->tok)
        ev->text.n = 0;
    if (p[i] != '<') {
        r = xml_scan_text(s, p, n, &i, ev);
    } else if (i + 1 >= n) {
        return XS_NEED;
    } else if (p[i + 1] == '/') {
        r = xml_scan_close(s, p, n, &i, ev);
    } else if (p[i + 1] == '!') {
        r = xml_scan_bang(s, p, n, &i, ev);
    } else if (p[i + 1] == '?') {
        r = xml_scan_pi(s, p, n, &i, ev);
    } else {
        size_t j = i + 1;
        ev->kind = XEV_OPEN;
        r = xml_scan_open(s, p, n, &j, ev);
        i = j;
    }
    if (r == XS_EVENT)
        s->pos = i;
    return r;
}

static const uint8_t XML_ESC[256] = {
    ['<'] = 1,
    ['>'] = 1,
    ['&'] = 1,
    ['"'] = 1,
};

static const uint8_t XML_ESC_ATTR[256] = {
    ['<'] = 1,
    ['>'] = 1,
    ['&'] = 1,
    ['"'] = 1,
    ['\t'] = 1,
    ['\n'] = 1,
    ['\r'] = 1,
};

static const uint8_t XML_ESC_SET[4] = { '<', '>', '&', '"' };

static const uint8_t XML_ESC_ATTR_SET[7] = { '<', '>', '&', '"', '\t', '\n', '\r' };

void xml_escape(xb_t* b, const char* s, size_t n, int in_attr)
{
    const uint8_t* tbl = in_attr ? XML_ESC_ATTR : XML_ESC;
    const uint8_t* set = in_attr ? XML_ESC_ATTR_SET : XML_ESC_SET;
    size_t setn = in_attr ? sizeof XML_ESC_ATTR_SET : sizeof XML_ESC_SET;
    size_t i = 0, run;
    while (i < n) {
        run = i;
        {
            size_t probe = i + XML_SIMD_MIN;
            if (probe > n)
                probe = n;
            while (i < probe && !tbl[(unsigned char)s[i]])
                i++;
            if (i == probe && i < n) {
                size_t k = simd.find_first_of((const uint8_t*)s + i, n - i,
                    set, setn);
                i = (k == (size_t)-1) ? n : i + k;
            }
        }
        xb_write(b, s + run, i - run);
        if (i >= n)
            break;
        switch (s[i]) {
        case '<':
            xb_puts(b, "&lt;");
            break;
        case '>':
            xb_puts(b, "&gt;");
            break;
        case '&':
            xb_puts(b, "&amp;");
            break;
        case '\t':
            xb_puts(b, "&#9;");
            break;
        case '\n':
            xb_puts(b, "&#10;");
            break;
        case '\r':
            xb_puts(b, "&#13;");
            break;
        default:
            xb_puts(b, in_attr ? "&quot;" : "\"");
            break;
        }
        i++;
    }
}
