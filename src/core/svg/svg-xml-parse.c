#include "svg-int.h"
#include <ctype.h>

typedef struct { const char* name; size_t nl; const char* val; size_t vl; } RawAttr;

static XMLNode* new_elem(const char* name, size_t nlen,
                         const RawAttr* at, int nat) {
    if (!name) return NULL;
    size_t bytes = sizeof(XMLNode) + (size_t)nat * 2 * sizeof(char*) + nlen + 1;
    for (int k = 0; k < nat; k++) bytes += at[k].nl + at[k].vl + 2;
    XMLNode* n = (XMLNode*)svg_malloc(bytes);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    char** attrs = (char**)(void*)(n + 1);
    char* str = (char*)(attrs + (size_t)nat * 2);
    memcpy(str, name, nlen);
    str[nlen] = 0;
    n->name = str;
    str += nlen + 1;
    for (int k = 0; k < nat; k++) {
        attrs[k * 2] = str;
        memcpy(str, at[k].name, at[k].nl);
        str[at[k].nl] = 0;
        str += at[k].nl + 1;
        attrs[k * 2 + 1] = str;
        memcpy(str, at[k].val, at[k].vl);
        str[at[k].vl] = 0;
        str += at[k].vl + 1;
    }
    n->attrs = nat ? attrs : NULL;
    n->nattrs = n->capattrs = nat;
    return n;
}

static int add_child(XMLNode* parent, XMLNode* child) {
    if (parent->nkids == parent->capkids) {
        int nc = parent->capkids ? parent->capkids * 2 : 4;
        XMLNode** nk = (XMLNode**)svg_realloc(parent->kids,
                                              (size_t)nc * sizeof(XMLNode*));
        if (!nk) return 0;
        parent->kids = nk;
        parent->capkids = nc;
    }
    child->parent = parent;
    parent->kids[parent->nkids++] = child;
    return 1;
}

static char* decode_entities_alloc(const char* s, size_t len, int scratch) {

    if (len > 0 && memchr(s, '&', len) == NULL) {
        char* out = scratch ? (char*)svg_scratch_alloc(len + 1)
                            : (char*)svg_malloc(len + 1);
        if (!out) return NULL;
        memcpy(out, s, len);
        out[len] = 0;
        return out;
    }
    size_t cap = len * 6 + 1;
    char* out = scratch ? (char*)svg_scratch_alloc(cap)
                        : (char*)svg_malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (s[i] == '&') {
            const char* semi = memchr(s + i, ';', len - i);
            if (semi && (size_t)(semi - (s + i)) <= 12) {
                char name[16];
                size_t nl = (size_t)(semi - (s + i + 1));
                if (nl < sizeof(name)) {
                    memcpy(name, s + i + 1, nl);
                    name[nl] = 0;
                    unsigned int cp = 0;
                    int ok = 0;
                    if (name[0] == '#') {
                        if (name[1] == 'x' || name[1] == 'X')
                            cp = (unsigned int)strtoul(name + 2, NULL, 16);
                        else
                            cp = (unsigned int)strtoul(name + 1, NULL, 10);
                        ok = 1;
                    } else if (!strcmp(name, "amp"))  { cp = '&';  ok = 1; }
                    else if (!strcmp(name, "lt"))   { cp = '<';  ok = 1; }
                    else if (!strcmp(name, "gt"))   { cp = '>';  ok = 1; }
                    else if (!strcmp(name, "quot")) { cp = '"';  ok = 1; }
                    else if (!strcmp(name, "apos")) { cp = '\''; ok = 1; }
                    else if (!strcmp(name, "nbsp")) { cp = 0xA0; ok = 1; }
                    if (ok) {
                        if (cp < 0x80) {
                            out[o++] = (char)cp;
                        } else if (cp < 0x800) {
                            out[o++] = (char)(0xC0 | (cp >> 6));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[o++] = (char)(0xF0 | (cp >> 18));
                            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                        i = (size_t)(semi - s) + 1;
                        continue;
                    }
                }
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0;
    return out;
}

static int node_preserves_space(const XMLNode* n) {
    for (const XMLNode* p = n; p; p = p->parent) {
        for (int i = 0; i < p->nattrs; i++) {
            const char* a = p->attrs[i * 2];
            if (strcmp(a, "xml:space") && strcmp(a, "space")) continue;
            const char* v = p->attrs[i * 2 + 1];
            if (!strcmp(v, "preserve")) return 1;
            if (!strcmp(v, "default")) return 0;
        }
    }
    return 0;
}

static void add_text(XMLNode* n, const char* s, size_t len) {

    int all_ws = 1;
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)s[i])) { all_ws = 0; break; }
    }
    if (all_ws && !node_preserves_space(n)) return;

    char* out = decode_entities_alloc(s, len, 0);
    if (!out) return;
    size_t o = strlen(out);

    if (n->text) {
        size_t a = strlen(n->text);
        char* t = (char*)svg_malloc(a + o + 1);
        if (t) {
            memcpy(t, n->text, a);
            memcpy(t + a, out, o + 1);
            svg_mfree(n->text);
            n->text = t;
        }
        svg_mfree(out);
    } else {
        n->text = out;
    }
}

XMLNode* svg_xml_parse(const char* data, size_t len) {
    if (!data) return NULL;
    XMLNode* doc = new_elem("#document", 9, NULL, 0);
    if (!doc) return NULL;

    XMLNode** stack = NULL;
    int sp = 0, scap = 0;
    size_t i = 0;

    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        i = 3;

    XMLNode* cur = doc;

    while (i < len) {
        if (data[i] != '<') {
            size_t j = i;
            while (j < len && data[j] != '<') j++;
            add_text(cur, data + i, j - i);
            i = j;
            continue;
        }

        if (i + 1 >= len) break;

        if (data[i + 1] == '?') {
            size_t j = i + 2;
            while (j + 1 < len && !(data[j] == '?' && data[j + 1] == '>')) j++;
            i = (j + 1 < len) ? j + 2 : len;
            continue;
        }

        if (data[i + 1] == '!') {
            if (i + 3 < len && data[i + 2] == '-' && data[i + 3] == '-') {
                size_t j = i + 4;
                while (j + 2 < len &&
                       !(data[j] == '-' && data[j + 1] == '-' && data[j + 2] == '>')) j++;
                i = (j + 2 < len) ? j + 3 : len;
            } else if (i + 8 < len && !memcmp(data + i + 2, "[CDATA[", 7)) {
                size_t j = i + 9;
                while (j + 2 < len &&
                       !(data[j] == ']' && data[j + 1] == ']' && data[j + 2] == '>')) j++;
                add_text(cur, data + i + 9, j - (i + 9));
                i = (j + 2 < len) ? j + 3 : len;
            } else {

                size_t j = i + 2;
                int depth = 0;
                while (j < len) {
                    if (data[j] == '[') depth++;
                    else if (data[j] == ']') { if (depth > 0) depth--; }
                    else if (data[j] == '>' && depth <= 0) break;
                    j++;
                }
                i = (j < len) ? j + 1 : len;
            }
            continue;
        }

        if (data[i + 1] == '/') {
            size_t ns = i + 2;
            size_t j = ns;
            while (j < len && !isspace((unsigned char)data[j]) && data[j] != '>') j++;
            size_t nlen = j - ns;
            while (j < len && data[j] != '>') j++;
            i = (j < len) ? j + 1 : len;
            while (sp > 0) {
                XMLNode* n = stack[--sp];
                if (n->name && strlen(n->name) == nlen &&
                    !strncmp(n->name, data + ns, nlen))
                    break;
            }
            cur = (sp > 0) ? stack[sp - 1] : doc;
            continue;
        }

        {
            size_t ns = i + 1;
            size_t j = ns;
            while (j < len && !isspace((unsigned char)data[j]) &&
                   data[j] != '/' && data[j] != '>') j++;

            size_t nmark = svg_scratch_mark();
            const char* name = data + ns;
            size_t nlen = j - ns;
            RawAttr local[16];
            RawAttr* at = local;
            int nat = 0, cat = 16, oom = 0;

            int selfclose = 0;
            while (j < len) {
                while (j < len && isspace((unsigned char)data[j])) j++;
                if (j >= len) break;
                if (data[j] == '>') { j++; break; }
                if (data[j] == '/') {
                    if (j + 1 < len && data[j + 1] == '>') { j += 2; selfclose = 1; break; }
                    j++;
                    continue;
                }
                size_t as = j;
                while (j < len && !isspace((unsigned char)data[j]) &&
                       data[j] != '=' && data[j] != '>' && data[j] != '/') j++;
                size_t anlen = j - as;
                while (j < len && isspace((unsigned char)data[j])) j++;

                const char* vs = NULL;
                size_t vlen = 0;
                if (j < len && data[j] == '=') {
                    j++;
                    while (j < len && isspace((unsigned char)data[j])) j++;
                    if (j < len && (data[j] == '"' || data[j] == '\'')) {
                        char q = data[j++];
                        vs = data + j;
                        while (j < len && data[j] != q) j++;
                        vlen = (size_t)(data + j - vs);
                        if (j < len) j++;
                    } else {
                        vs = data + j;
                        while (j < len && !isspace((unsigned char)data[j]) &&
                               data[j] != '>') j++;
                        vlen = (size_t)(data + j - vs);
                    }
                }
                if (anlen == 0 || oom) continue;
                if (nat == cat) {

                    RawAttr* na = (RawAttr*)svg_scratch_alloc((size_t)cat * 2 * sizeof(RawAttr));
                    if (!na) { oom = 1; continue; }
                    memcpy(na, at, (size_t)nat * sizeof(RawAttr));
                    at = na;
                    cat *= 2;
                }
                const char* val = "";
                size_t vl = 0;
                if (vs) {
                    char* dec = decode_entities_alloc(vs, vlen, 1);

                    if (dec) { val = dec; vl = strlen(dec); }
                }
                at[nat].name = data + as;
                at[nat].nl = anlen;
                at[nat].val = val;
                at[nat].vl = vl;
                nat++;
            }
            XMLNode* el = new_elem(name, nlen, at, nat);
            svg_scratch_release(nmark);
            if (!el) break;
            i = j;

            if (!add_child(cur, el)) { xml_free(el); break; }
            if (!selfclose) {
                if (sp == scap) {
                    int nscap = scap ? scap * 2 : 16;
                    XMLNode** nsx = (XMLNode**)svg_realloc(stack, (size_t)nscap * sizeof(XMLNode*));
                    if (!nsx) {

                        break;
                    }
                    stack = nsx; scap = nscap;
                }
                stack[sp++] = el;
                cur = el;
            }
        }
    }

    svg_mfree(stack);
    return doc;
}
