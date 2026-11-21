#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

dyn_scres_t* dyn_sc_res_find(dyn_schema_t* sch, const char* uri, size_t ulen)
{
    int i;
    for (i = 0; i < sch->n_res; i++)
        if (strlen(sch->res[i]->uri) == ulen && !memcmp(sch->res[i]->uri, uri, ulen))
            return sch->res[i];
    return NULL;
}

dyn_scres_t* dyn_sc_res_add(dyn_sccomp_t* c, const char* uri, size_t ulen,
    const char* prefix, size_t plen, dyn_scnode_t* root)
{
    dyn_schema_t* sch = c->sch;
    dyn_scres_t* r;
    if (sch->n_res >= sch->cap_res) {
        int ncap = sch->cap_res ? sch->cap_res * 2 : 8;
        dyn_scres_t** nr = js_realloc(c->ctx, sch->res, (size_t)ncap * sizeof(*nr));
        if (!nr)
            return NULL;
        sch->res = nr;
        sch->cap_res = ncap;
    }
    r = js_mallocz(c->ctx, sizeof(*r));
    if (!r)
        return NULL;
    r->uri = js_malloc(c->ctx, ulen + 1);
    r->prefix = js_malloc(c->ctx, plen + 1);
    if (!r->uri || !r->prefix) {
        js_free(c->ctx, r->uri);
        js_free(c->ctx, r->prefix);
        js_free(c->ctx, r);
        return NULL;
    }
    memcpy(r->uri, uri, ulen);
    r->uri[ulen] = 0;
    memcpy(r->prefix, prefix, plen);
    r->prefix[plen] = 0;
    r->root = root;
    sch->res[sch->n_res++] = r;
    return r;
}

int dyn_sc_res_anchor_add(dyn_sccomp_t* c, dyn_scres_t* r, const char* name,
    dyn_scnode_t* n, int dynamic)
{
    int i;
    char** nn;
    dyn_scnode_t** nd;
    int* ny;
    for (i = 0; i < r->na; i++)
        if (!strcmp(r->anames[i], name)) {
            if (r->anodes[i] == n) {
                r->adyn[i] |= dynamic;
                return 0;
            }
            JS_ThrowTypeError(c->ctx, "dyna:schema: duplicate anchor \"%s\" in resource \"%s\"", name, r->uri);
            return -1;
        }
    nn = js_realloc(c->ctx, r->anames, (size_t)(r->na + 1) * sizeof(*nn));
    if (!nn)
        return -1;
    r->anames = nn;
    nd = js_realloc(c->ctx, r->anodes, (size_t)(r->na + 1) * sizeof(*nd));
    if (!nd)
        return -1;
    r->anodes = nd;
    ny = js_realloc(c->ctx, r->adyn, (size_t)(r->na + 1) * sizeof(*ny));
    if (!ny)
        return -1;
    r->adyn = ny;
    r->anames[r->na] = js_strdup(c->ctx, name);
    if (!r->anames[r->na])
        return -1;
    r->anodes[r->na] = n;
    r->adyn[r->na] = dynamic;
    r->na++;
    return 0;
}

dyn_scnode_t* dyn_sc_res_anchor(const dyn_scres_t* r, const char* name, int dynamic_only)
{
    int i;
    for (i = 0; i < r->na; i++)
        if (!strcmp(r->anames[i], name) && (!dynamic_only || r->adyn[i]))
            return r->anodes[i];
    return NULL;
}

void dyn_sc_res_free_rt(JSRuntime* rt, dyn_scres_t* r)
{
    int i;
    for (i = 0; i < r->na; i++)
        js_free_rt(rt, r->anames[i]);
    js_free_rt(rt, r->anames);
    js_free_rt(rt, r->anodes);
    js_free_rt(rt, r->adyn);
    js_free_rt(rt, r->uri);
    js_free_rt(rt, r->prefix);
    js_free_rt(rt, r);
}

static const struct {
    const char* uri;
    const char* json;
} dyn_sc_builtin_docs[] = {
    { "https:/\057json-schema.org/draft/2020-12/schema",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/schema\",\"$vocabulary\":{\"https:/\057json-schema.org/draft/2020-12/vocab/core\":true,\"http"
        "s:/\057json-schema.org/draft/2020-12/vocab/applicator\":true,\"https:/\057json-schema.org/draft/2020-12/vo"
        "cab/unevaluated\":true,\"https:/\057json-schema.org/draft/2020-12/vocab/validation\":true,\"https:/\057jso"
        "n-schema.org/draft/2020-12/vocab/meta-data\":true,\"https:/\057json-schema.org/draft/2020-12/vocab/form"
        "at-annotation\":true,\"https:/\057json-schema.org/draft/2020-12/vocab/content\":true},\"$dynamicAnchor\":\"meta\",\"title\":\"Core and Validation specifications meta-schema\",\"allOf\":[{\"$ref\":\"meta/"
        "core\"},{\"$ref\":\"meta/applicator\"},{\"$ref\":\"meta/unevaluated\"},{\"$ref\":\"meta/validation\""
        "},{\"$ref\":\"meta/meta-data\"},{\"$ref\":\"meta/format-annotation\"},{\"$ref\":\"meta/content\"}],\"type\":[\"object\",\"boolean\"],\"$comment\":\"This meta-schema also defines keywords that have app"
        "eared in previous drafts in order to prevent incompatible extensions as they remain in common use.\""
        ",\"properties\":{\"definitions\":{\"$comment\":\"\\\"definitions\\\" has been replaced by \\\"$defs\\\".\",\"type\":\"object\",\"additionalProperties\":{\"$dynamicRef\":\"#meta\"},\"deprecated\":true,"
        "\"default\":{}},\"dependencies\":{\"$comment\":\"\\\"dependencies\\\" has been split and replaced by"
        " \\\"dependentSchemas\\\" and \\\"dependentRequired\\\" in order to serve their differing semantics."
        "\",\"type\":\"object\",\"additionalProperties\":{\"anyOf\":[{\"$dynamicRef\":\"#meta\"},{\"$ref\":\""
        "meta/validation#/$defs/stringArray\"}]},\"deprecated\":true,\"default\":{}},\"$recursiveAnchor\":{\""
        "$comment\":\"\\\"$recursiveAnchor\\\" has been replaced by \\\"$dynamicAnchor\\\".\",\"$ref\":\"meta"
        "/core#/$defs/anchorString\",\"deprecated\":true},\"$recursiveRef\":{\"$comment\":\"\\\"$recursiveRef"
        "\\\" has been replaced by \\\"$dynamicRef\\\".\",\"$ref\":\"meta/core#/$defs/uriReferenceString\",\""
        "deprecated\":true}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/core",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/core\",\"$dynamicAnchor\":\"meta\",\"title\":\"Core vocabulary meta-schema\",\"type\""
        ":[\"object\",\"boolean\"],\"properties\":{\"$id\":{\"$ref\":\"#/$defs/uriReferenceString\",\"$commen"
        "t\":\"Non-empty fragments not allowed.\",\"pattern\":\"^[^#]*#?$\"},\"$schema\":{\"$ref\":\"#/$defs/"
        "uriString\"},\"$ref\":{\"$ref\":\"#/$defs/uriReferenceString\"},\"$anchor\":{\"$ref\":\"#/$defs/anch"
        "orString\"},\"$dynamicRef\":{\"$ref\":\"#/$defs/uriReferenceString\"},\"$dynamicAnchor\":{\"$ref\":\"#/$defs/anchorString\"},\"$vocabulary\":{\"type\":\"object\",\"propertyNames\":{\"$ref\":\"#/$defs/"
        "uriString\"},\"additionalProperties\":{\"type\":\"boolean\"}},\"$comment\":{\"type\":\"string\"},\"$"
        "defs\":{\"type\":\"object\",\"additionalProperties\":{\"$dynamicRef\":\"#meta\"}}},\"$defs\":{\"anch"
        "orString\":{\"type\":\"string\",\"pattern\":\"^[A-Za-z_][-A-Za-z0-9._]*$\"},\"uriString\":{\"type\":"
        "\"string\",\"format\":\"uri\"},\"uriReferenceString\":{\"type\":\"string\",\"format\":\"uri-referenc"
        "e\"}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/applicator",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/applicator\",\"$dynamicAnchor\":\"meta\",\"title\":\"Applicator vocabulary meta-schem"
        "a\",\"type\":[\"object\",\"boolean\"],\"properties\":{\"prefixItems\":{\"$ref\":\"#/$defs/schemaArra"
        "y\"},\"items\":{\"$dynamicRef\":\"#meta\"},\"contains\":{\"$dynamicRef\":\"#meta\"},\"additionalProp"
        "erties\":{\"$dynamicRef\":\"#meta\"},\"properties\":{\"type\":\"object\",\"additionalProperties\":{\"$dynamicRef\":\"#meta\"},\"default\":{}},\"patternProperties\":{\"type\":\"object\",\"additionalPro"
        "perties\":{\"$dynamicRef\":\"#meta\"},\"propertyNames\":{\"format\":\"regex\"},\"default\":{}},\"dep"
        "endentSchemas\":{\"type\":\"object\",\"additionalProperties\":{\"$dynamicRef\":\"#meta\"},\"default\":{}},\"propertyNames\":{\"$dynamicRef\":\"#meta\"},\"if\":{\"$dynamicRef\":\"#meta\"},\"then\":{\"$"
        "dynamicRef\":\"#meta\"},\"else\":{\"$dynamicRef\":\"#meta\"},\"allOf\":{\"$ref\":\"#/$defs/schemaArr"
        "ay\"},\"anyOf\":{\"$ref\":\"#/$defs/schemaArray\"},\"oneOf\":{\"$ref\":\"#/$defs/schemaArray\"},\"no"
        "t\":{\"$dynamicRef\":\"#meta\"}},\"$defs\":{\"schemaArray\":{\"type\":\"array\",\"minItems\":1,\"ite"
        "ms\":{\"$dynamicRef\":\"#meta\"}}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/unevaluated",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/unevaluated\",\"$dynamicAnchor\":\"meta\",\"title\":\"Unevaluated applicator vocabula"
        "ry meta-schema\",\"type\":[\"object\",\"boolean\"],\"properties\":{\"unevaluatedItems\":{\"$dynamicR"
        "ef\":\"#meta\"},\"unevaluatedProperties\":{\"$dynamicRef\":\"#meta\"}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/validation",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/validation\",\"$dynamicAnchor\":\"meta\",\"title\":\"Validation vocabulary meta-schem"
        "a\",\"type\":[\"object\",\"boolean\"],\"properties\":{\"type\":{\"anyOf\":[{\"$ref\":\"#/$defs/simpl"
        "eTypes\"},{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/simpleTypes\"},\"minItems\":1,\"uniqueIt"
        "ems\":true}]},\"const\":true,\"enum\":{\"type\":\"array\",\"items\":true},\"multipleOf\":{\"type\":\"number\",\"exclusiveMinimum\":0},\"maximum\":{\"type\":\"number\"},\"exclusiveMaximum\":{\"type\":\"number\"},\"minimum\":{\"type\":\"number\"},\"exclusiveMinimum\":{\"type\":\"number\"},\"maxLength\":{\"$ref\":\"#/$defs/nonNegativeInteger\"},\"minLength\":{\"$ref\":\"#/$defs/nonNegativeIntegerDefa"
        "ult0\"},\"pattern\":{\"type\":\"string\",\"format\":\"regex\"},\"maxItems\":{\"$ref\":\"#/$defs/nonN"
        "egativeInteger\"},\"minItems\":{\"$ref\":\"#/$defs/nonNegativeIntegerDefault0\"},\"uniqueItems\":{\""
        "type\":\"boolean\",\"default\":false},\"maxContains\":{\"$ref\":\"#/$defs/nonNegativeInteger\"},\"mi"
        "nContains\":{\"$ref\":\"#/$defs/nonNegativeInteger\",\"default\":1},\"maxProperties\":{\"$ref\":\"#/"
        "$defs/nonNegativeInteger\"},\"minProperties\":{\"$ref\":\"#/$defs/nonNegativeIntegerDefault0\"},\"re"
        "quired\":{\"$ref\":\"#/$defs/stringArray\"},\"dependentRequired\":{\"type\":\"object\",\"additionalP"
        "roperties\":{\"$ref\":\"#/$defs/stringArray\"}}},\"$defs\":{\"nonNegativeInteger\":{\"type\":\"integ"
        "er\",\"minimum\":0},\"nonNegativeIntegerDefault0\":{\"$ref\":\"#/$defs/nonNegativeInteger\",\"defaul"
        "t\":0},\"simpleTypes\":{\"enum\":[\"array\",\"boolean\",\"integer\",\"null\",\"number\",\"object\",\"string\"]},\"stringArray\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"uniqueItems\":true"
        ",\"default\":[]}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/meta-data",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/meta-data\",\"$dynamicAnchor\":\"meta\",\"title\":\"Meta-data vocabulary meta-schema\",\"type\":[\"object\",\"boolean\"],\"properties\":{\"title\":{\"type\":\"string\"},\"description\":"
        "{\"type\":\"string\"},\"default\":true,\"deprecated\":{\"type\":\"boolean\",\"default\":false},\"rea"
        "dOnly\":{\"type\":\"boolean\",\"default\":false},\"writeOnly\":{\"type\":\"boolean\",\"default\":fal"
        "se},\"examples\":{\"type\":\"array\",\"items\":true}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/format-annotation",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/format-annotation\",\"$dynamicAnchor\":\"meta\",\"title\":\"Format vocabulary meta-sc"
        "hema for annotation results\",\"type\":[\"object\",\"boolean\"],\"properties\":{\"format\":{\"type\""
        ":\"string\"}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/format-assertion",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/format-assertion\",\"$dynamicAnchor\":\"meta\",\"title\":\"Format vocabulary meta-sch"
        "ema for assertion results\",\"type\":[\"object\",\"boolean\"],\"properties\":{\"format\":{\"type\":\"string\"}}}" },
    { "https:/\057json-schema.org/draft/2020-12/meta/content",
        "{\"$schema\":\"https:/\057json-schema.org/draft/2020-12/schema\",\"$id\":\"https:/\057json-schema.org/draf"
        "t/2020-12/meta/content\",\"$dynamicAnchor\":\"meta\",\"title\":\"Content vocabulary meta-schema\",\""
        "type\":[\"object\",\"boolean\"],\"properties\":{\"contentEncoding\":{\"type\":\"string\"},\"contentM"
        "ediaType\":{\"type\":\"string\"},\"contentSchema\":{\"$dynamicRef\":\"#meta\"}}}" },
};

int dyn_sc_load_doc(dyn_sccomp_t* c, const char* uri, size_t ulen)
{
    JSContext* ctx = c->ctx;
    JSValue doc = JS_UNDEFINED;
    char *key, prefix[32];
    const char* saved_base = c->base_uri;
    dyn_scres_t* saved_cur = c->cur;
    dyn_scnode_t* root = NULL;
    size_t i, plen;
    int rc;

    key = js_malloc(ctx, ulen + 1);
    if (!key)
        return -1;
    memcpy(key, uri, ulen);
    key[ulen] = 0;
    if (JS_IsObject(c->resources)) {
        doc = JS_GetPropertyStr(ctx, c->resources, key);
        if (JS_IsException(doc)) {
            js_free(ctx, key);
            return -1;
        }
    }
    for (i = 0; JS_IsUndefined(doc) && i < countof(dyn_sc_builtin_docs); i++)
        if (!strcmp(dyn_sc_builtin_docs[i].uri, key)) {
            const char* j = dyn_sc_builtin_docs[i].json;
            doc = JS_ParseJSON(ctx, j, strlen(j), "<dyna:schema meta>");
            if (JS_IsException(doc)) {
                js_free(ctx, key);
                return -1;
            }
        }
    if (JS_IsUndefined(doc)) {
        js_free(ctx, key);
        return 0;
    }
    plen = (size_t)snprintf(prefix, sizeof prefix, "\001%d", ++c->n_docs);
    c->base_uri = key;
    c->cur = NULL;
    rc = dyn_sc_compile(c, doc, prefix, plen, &root);
    c->base_uri = saved_base;
    c->cur = saved_cur;
    if (c->ndocv < (int)countof(c->docv)) {
        memcpy(c->docp[c->ndocv], prefix, plen + 1);
        c->docv[c->ndocv++] = doc;
    } else {
        JS_FreeValue(ctx, doc);
    }
    if (!rc && !dyn_sc_res_find(c->sch, key, ulen) && !dyn_sc_res_add(c, key, ulen, prefix, plen, root))
        rc = -1;
    js_free(ctx, key);
    return rc ? -1 : 1;
}

#endif
