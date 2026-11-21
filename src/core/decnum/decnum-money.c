#include "decnum.h"

static const struct {
    const char* code;
    uint8_t minor;
} MONEY_MINOR[] = {
    { "JPY", 0 },
    { "KRW", 0 },
    { "VND", 0 },
    { "CLP", 0 },
    { "ISK", 0 },
    { "PYG", 0 },
    { "RWF", 0 },
    { "UGX", 0 },
    { "VUV", 0 },
    { "XAF", 0 },
    { "XOF", 0 },
    { "XPF", 0 },
    { "DJF", 0 },
    { "GNF", 0 },
    { "KMF", 0 },
    { "MGA", 0 },
    { "BIF", 0 },
    { "BHD", 3 },
    { "IQD", 3 },
    { "JOD", 3 },
    { "KWD", 3 },
    { "LYD", 3 },
    { "OMR", 3 },
    { "TND", 3 },
};

uint8_t money_minor(const char* code)
{
    size_t k;
    for (k = 0; k < countof(MONEY_MINOR); k++)
        if (strcmp(MONEY_MINOR[k].code, code) == 0)
            return MONEY_MINOR[k].minor;
    return 2;
}
