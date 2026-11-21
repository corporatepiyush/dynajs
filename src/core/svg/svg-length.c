#include "svg-int.h"
#include <ctype.h>

float length_parse(const char* s, float vw, float vh, float em, int axis, float def) {
    if (!s) return def;
    while (isspace((unsigned char)*s)) s++;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s) return def;
    while (isspace((unsigned char)*end)) end++;

    if (*end == 0 || !strcmp(end, "px")) return v;
    if (!strcmp(end, "pt")) return v * (96.0f / 72.0f);
    if (!strcmp(end, "pc")) return v * 16.0f;
    if (!strcmp(end, "in")) return v * 96.0f;
    if (!strcmp(end, "cm")) return v * (96.0f / 2.54f);
    if (!strcmp(end, "mm")) return v * (96.0f / 25.4f);
    if (!strcmp(end, "em")) return v * em;
    if (!strcmp(end, "ex")) return v * em * 0.5f;
    if (*end == '%') {

        float ref;
        if (axis == 0) ref = vw;
        else if (axis == 1) ref = vh;
        else ref = sqrtf(vw * vw + vh * vh) / sqrtf(2.0f);
        return v * ref / 100.0f;
    }
    return v;
}
