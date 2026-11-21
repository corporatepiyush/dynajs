#include "svg-int.h"
#include <ctype.h>

typedef struct { const char* name; unsigned int rgb; } NamedColor;

static const NamedColor k_named_colors[] = {
    {"aliceblue",0xf0f8ff}, {"antiquewhite",0xfaebd7}, {"aqua",0x00ffff},
    {"aquamarine",0x7fffd4}, {"azure",0xf0ffff}, {"beige",0xf5f5dc},
    {"bisque",0xffe4c4}, {"black",0x000000}, {"blanchedalmond",0xffebcd},
    {"blue",0x0000ff}, {"blueviolet",0x8a2be2}, {"brown",0xa52a2a},
    {"burlywood",0xdeb887}, {"cadetblue",0x5f9ea0}, {"chartreuse",0x7fff00},
    {"chocolate",0xd2691e}, {"coral",0xff7f50}, {"cornflowerblue",0x6495ed},
    {"cornsilk",0xfff8dc}, {"crimson",0xdc143c}, {"cyan",0x00ffff},
    {"darkblue",0x00008b}, {"darkcyan",0x008b8b}, {"darkgoldenrod",0xb8860b},
    {"darkgray",0xa9a9a9}, {"darkgreen",0x006400}, {"darkgrey",0xa9a9a9},
    {"darkkhaki",0xbdb76b}, {"darkmagenta",0x8b008b}, {"darkolivegreen",0x556b2f},
    {"darkorange",0xff8c00}, {"darkorchid",0x9932cc}, {"darkred",0x8b0000},
    {"darksalmon",0xe9967a}, {"darkseagreen",0x8fbc8f}, {"darkslateblue",0x483d8b},
    {"darkslategray",0x2f4f4f}, {"darkslategrey",0x2f4f4f}, {"darkturquoise",0x00ced1},
    {"darkviolet",0x9400d3}, {"deeppink",0xff1493}, {"deepskyblue",0x00bfff},
    {"dimgray",0x696969}, {"dimgrey",0x696969}, {"dodgerblue",0x1e90ff},
    {"firebrick",0xb22222}, {"floralwhite",0xfffaf0}, {"forestgreen",0x228b22},
    {"fuchsia",0xff00ff}, {"gainsboro",0xdcdcdc}, {"ghostwhite",0xf8f8ff},
    {"gold",0xffd700}, {"goldenrod",0xdaa520}, {"gray",0x808080},
    {"green",0x008000}, {"greenyellow",0xadff2f}, {"grey",0x808080},
    {"honeydew",0xf0fff0}, {"hotpink",0xff69b4}, {"indianred",0xcd5c5c},
    {"indigo",0x4b0082}, {"ivory",0xfffff0}, {"khaki",0xf0e68c},
    {"lavender",0xe6e6fa}, {"lavenderblush",0xfff0f5}, {"lawngreen",0x7cfc00},
    {"lemonchiffon",0xfffacd}, {"lightblue",0xadd8e6}, {"lightcoral",0xf08080},
    {"lightcyan",0xe0ffff}, {"lightgoldenrodyellow",0xfafad2}, {"lightgray",0xd3d3d3},
    {"lightgreen",0x90ee90}, {"lightgrey",0xd3d3d3}, {"lightpink",0xffb6c1},
    {"lightsalmon",0xffa07a}, {"lightseagreen",0x20b2aa}, {"lightskyblue",0x87cefa},
    {"lightslategray",0x778899}, {"lightslategrey",0x778899}, {"lightsteelblue",0xb0c4de},
    {"lightyellow",0xffffe0}, {"lime",0x00ff00}, {"limegreen",0x32cd32},
    {"linen",0xfaf0e6}, {"magenta",0xff00ff}, {"maroon",0x800000},
    {"mediumaquamarine",0x66cdaa}, {"mediumblue",0x0000cd}, {"mediumorchid",0xba55d3},
    {"mediumpurple",0x9370db}, {"mediumseagreen",0x3cb371}, {"mediumslateblue",0x7b68ee},
    {"mediumspringgreen",0x00fa9a}, {"mediumturquoise",0x48d1cc},
    {"mediumvioletred",0xc71585}, {"midnightblue",0x191970}, {"mintcream",0xf5fffa},
    {"mistyrose",0xffe4e1}, {"moccasin",0xffe4b5}, {"navajowhite",0xffdead},
    {"navy",0x000080}, {"oldlace",0xfdf5e6}, {"olive",0x808000},
    {"olivedrab",0x6b8e23}, {"orange",0xffa500}, {"orangered",0xff4500},
    {"orchid",0xda70d6}, {"palegoldenrod",0xeee8aa}, {"palegreen",0x98fb98},
    {"paleturquoise",0xafeeee}, {"palevioletred",0xdb7093}, {"papayawhip",0xffefd5},
    {"peachpuff",0xffdab9}, {"peru",0xcd853f}, {"pink",0xffc0cb},
    {"plum",0xdda0dd}, {"powderblue",0xb0e0e6}, {"purple",0x800080},
    {"rebeccapurple",0x663399}, {"red",0xff0000}, {"rosybrown",0xbc8f8f},
    {"royalblue",0x4169e1}, {"saddlebrown",0x8b4513}, {"salmon",0xfa8072},
    {"sandybrown",0xf4a460}, {"seagreen",0x2e8b57}, {"seashell",0xfff5ee},
    {"sienna",0xa0522d}, {"silver",0xc0c0c0}, {"skyblue",0x87ceeb},
    {"slateblue",0x6a5acd}, {"slategray",0x708090}, {"slategrey",0x708090},
    {"snow",0xfffafa}, {"springgreen",0x00ff7f}, {"steelblue",0x4682b4},
    {"tan",0xd2b48c}, {"teal",0x008080}, {"thistle",0xd8bfd8},
    {"tomato",0xff6347}, {"turquoise",0x40e0d0}, {"violet",0xee82ee},
    {"wheat",0xf5deb3}, {"white",0xffffff}, {"whitesmoke",0xf5f5f5},
    {"yellow",0xffff00}, {"yellowgreen",0x9acd32}
};

#define NAMED_COLOR_MAX 20

static const NamedColor* named_color_find(const char* s) {
    char key[NAMED_COLOR_MAX + 1];
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (n >= NAMED_COLOR_MAX) return NULL;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        else if (c < 'a' || c > 'z') return NULL;
        key[n] = (char)c;
    }
    key[n] = 0;
    size_t lo = 0, hi = sizeof(k_named_colors) / sizeof(k_named_colors[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(key, k_named_colors[mid].name);
        if (cmp == 0) return &k_named_colors[mid];
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char* s, Color* out) {
    int n = 0;
    while (hex_val((unsigned char)s[n]) >= 0) n++;
    if (n == 3 || n == 4) {
        int r = hex_val((unsigned char)s[0]);
        int g = hex_val((unsigned char)s[1]);
        int b = hex_val((unsigned char)s[2]);
        int a = (n == 4) ? hex_val((unsigned char)s[3]) : 15;
        out->r = (float)(r * 17) / 255.0f;
        out->g = (float)(g * 17) / 255.0f;
        out->b = (float)(b * 17) / 255.0f;
        out->a = (float)(a * 17) / 255.0f;
        return 1;
    }
    if (n == 6 || n == 8) {
        int r = hex_val((unsigned char)s[0]) * 16 + hex_val((unsigned char)s[1]);
        int g = hex_val((unsigned char)s[2]) * 16 + hex_val((unsigned char)s[3]);
        int b = hex_val((unsigned char)s[4]) * 16 + hex_val((unsigned char)s[5]);
        int a = (n == 8) ? hex_val((unsigned char)s[6]) * 16 + hex_val((unsigned char)s[7]) : 255;
        out->r = (float)r / 255.0f;
        out->g = (float)g / 255.0f;
        out->b = (float)b / 255.0f;
        out->a = (float)a / 255.0f;
        return 1;
    }
    return 0;
}

static int parse_args_scale(const char* s, float* v, int max,
                            float percent_scale) {
    const char* p = strchr(s, '(');
    const char* e = strrchr(s, ')');
    if (!p || !e || e <= p) return 0;
    char* inner = svg_strndup(p + 1, (size_t)(e - (p + 1)));
    if (!inner) return 0;
    int n = 0;
    char* q = inner;
    while (*q && n < max) {
        while (*q && (isspace((unsigned char)*q) || *q == ',' || *q == '/')) q++;
        if (!*q) break;
        char* end = NULL;
        float f = strtof(q, &end);
        if (end == q) break;
        q = end;
        int pct = 0;
        while (isspace((unsigned char)*q)) q++;
        if (*q == '%') { pct = 1; q++; }
        v[n++] = pct ? f / percent_scale : f;
    }
    svg_mfree(inner);
    return n;
}

static int parse_args(const char* s, float* v, int max) {
    return parse_args_scale(s, v, max, 100.0f);
}

static int parse_rgb(const char* s, Color* out) {

    const char* open = strchr(s, '(');
    const char* close = open ? strchr(open, ')') : NULL;
    if (!open || !close || close <= open) return 0;

    char* inner = svg_strndup(open + 1, (size_t)(close - (open + 1)));
    if (!inner) return 0;

    float v[4] = {0, 0, 0, 1};
    int n = 0, saw_fraction = 0;
    char* q = inner;
    while (*q && n < 4) {
        while (*q && (isspace((unsigned char)*q) || *q == ',' || *q == '/')) q++;
        if (!*q) break;
        const char* tok = q;
        char* end = NULL;
        float f = strtof(q, &end);
        if (end == q) { svg_mfree(inner); return 0; }

        for (const char* t = tok; t < end; t++) {
            if (*t == '.' || *t == 'e' || *t == 'E') { saw_fraction = 1; break; }
        }
        q = end;
        int pct = 0;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '%') { pct = 1; q++; }
        if (pct) {

            v[n++] = f / 100.0f;
            saw_fraction = 1;
        } else {
            v[n++] = f;
        }
    }
    svg_mfree(inner);
    if (n < 3) return 0;
    if (!saw_fraction)
        for (int i = 0; i < n && i < 3; i++) v[i] /= 255.0f;

    out->r = v[0] < 0.0f ? 0.0f : (v[0] > 1.0f ? 1.0f : v[0]);
    out->g = v[1] < 0.0f ? 0.0f : (v[1] > 1.0f ? 1.0f : v[1]);
    out->b = v[2] < 0.0f ? 0.0f : (v[2] > 1.0f ? 1.0f : v[2]);
    out->a = (n >= 4) ? v[3] : 1.0f;
    return 1;
}

static int parse_hsl(const char* s, Color* out) {
    float v[4] = {0, 0, 0, 1};
    int n = parse_args(s, v, 4);
    if (n < 3) return 0;
    float h = v[0], sat = v[1], light = v[2];
    out->a = (n >= 4) ? v[3] : 1.0f;

    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    sat = sat < 0.0f ? 0.0f : (sat > 1.0f ? 1.0f : sat);
    light = light < 0.0f ? 0.0f : (light > 1.0f ? 1.0f : light);

    float c = (1.0f - fabsf(2.0f * light - 1.0f)) * sat;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if      (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    float m = light - c * 0.5f;
    out->r = r + m;
    out->g = g + m;
    out->b = b + m;
    return 1;
}

int color_parse(const char* s, Color* out) {
    if (!s) return 0;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return 0;

    if (!strcmp(s, "transparent")) {
        out->r = out->g = out->b = out->a = 0.0f;
        return 1;
    }
    if (s[0] == '#') return parse_hex(s + 1, out);
    if (strncmp(s, "rgb", 3) == 0) return parse_rgb(s, out);
    if (strncmp(s, "hsl", 3) == 0) return parse_hsl(s, out);

    const NamedColor* nc = named_color_find(s);
    if (!nc) return 0;
    unsigned int rgb = nc->rgb;
    out->r = (float)((rgb >> 16) & 0xFF) / 255.0f;
    out->g = (float)((rgb >> 8) & 0xFF) / 255.0f;
    out->b = (float)(rgb & 0xFF) / 255.0f;
    out->a = 1.0f;
    return 1;
}
