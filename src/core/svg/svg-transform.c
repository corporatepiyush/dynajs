#include "svg-doc.h"
#include <ctype.h>
#include <math.h>

Mat parse_transform(const char* s) {
    Mat m = mat_identity();
    if (!s) return m;
    const char* p = s;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char name[32];
        int nl = 0;
        while (*p && !isspace((unsigned char)*p) && *p != '(' && nl < 31) name[nl++] = *p++;
        name[nl] = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '(') break;
        p++;
        float a[6] = {0, 0, 0, 0, 0, 0};
        int na = 0;
        while (*p && *p != ')' && na < 6) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
            if (*p == ')' || !*p) break;
            char* end = NULL;
            a[na] = strtof(p, &end);
            if (end == p) break;
            p = end;
            na++;
        }
        if (*p == ')') p++;

        Mat t = mat_identity();
        if      (!strcmp(name, "matrix") && na >= 6)
            t = (Mat){{a[0], a[1], a[2], a[3], a[4], a[5]}};
        else if (!strcmp(name, "translate") && na >= 1)
            t = mat_translate(a[0], na >= 2 ? a[1] : 0.0f);
        else if (!strcmp(name, "scale") && na >= 1)
            t = mat_scale(a[0], na >= 2 ? a[1] : a[0]);
        else if (!strcmp(name, "rotate") && na >= 1) {
            if (na >= 3)
                t = mat_mul(mat_translate(a[1], a[2]),
                            mat_mul(mat_rotate(a[0]), mat_translate(-a[1], -a[2])));
            else
                t = mat_rotate(a[0]);
        } else if (!strcmp(name, "skewX") && na >= 1) {
            t = (Mat){{1, 0, tanf(a[0] * SVG_PI / 180.0f), 1, 0, 0}};
        } else if (!strcmp(name, "skewY") && na >= 1) {
            t = (Mat){{1, tanf(a[0] * SVG_PI / 180.0f), 0, 1, 0, 0}};
        }
        m = mat_mul(m, t);
    }
    return m;
}

Mat transform_attr(const XMLNode* xn) {
    return parse_transform(xml_attr(xn, "transform"));
}
