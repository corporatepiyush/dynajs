#include "timek.h"
#include "../dyn-ctype.h"

static const char* const dyn_dp_en_months[12] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december"
};

static const char* const dyn_dp_en_abbr[12] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec"
};

static const char* const dyn_dp_en_days[7] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"
};

static const char* const dyn_dp_fr_months[12] = {
    "janvier", "fevrier", "mars", "avril", "mai", "juin",
    "juillet", "aout", "septembre", "octobre", "novembre", "decembre"
};

static const char* const dyn_dp_fr_abbr[12] = {
    "jan", "fev", "mar", "avr", "mai", "jui",
    "jul", "aou", "sep", "oct", "nov", "dec"
};

static const char* const dyn_dp_fr_days[7] = {
    "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"
};

static const char* const dyn_dp_de_months[12] = {
    "januar", "februar", "marz", "april", "mai", "juni",
    "juli", "august", "september", "oktober", "november", "dezember"
};

static const char* const dyn_dp_de_abbr[12] = {
    "jan", "feb", "mar", "apr", "mai", "jun",
    "jul", "aug", "sep", "okt", "nov", "dez"
};

static const char* const dyn_dp_de_days[7] = {
    "sonntag", "montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag"
};

static const char* const dyn_dp_es_months[12] = {
    "enero", "febrero", "marzo", "abril", "mayo", "junio",
    "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"
};

static const char* const dyn_dp_es_abbr[12] = {
    "ene", "feb", "mar", "abr", "may", "jun",
    "jul", "ago", "sep", "oct", "nov", "dic"
};

static const char* const dyn_dp_es_days[7] = {
    "domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"
};

static const DynDateLocale dyn_dp_locales[] = {
    { "en-US", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 0 },
    { "en-GB", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 1 },
    { "en", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 0 },
    { "fr", dyn_dp_fr_months, dyn_dp_fr_abbr, dyn_dp_fr_days, 1 },
    { "de", dyn_dp_de_months, dyn_dp_de_abbr, dyn_dp_de_days, 1 },
    { "es", dyn_dp_es_months, dyn_dp_es_abbr, dyn_dp_es_days, 1 },
};

const DynDateLocale* dyn_dp_locale_find(const char* name)
{
    size_t i;

    if (!name)
        return &dyn_dp_locales[0];
    for (i = 0; i < sizeof(dyn_dp_locales) / sizeof(dyn_dp_locales[0]); i++)
        if (strcmp(name, dyn_dp_locales[i].name) == 0)
            return &dyn_dp_locales[i];
    return NULL;
}
