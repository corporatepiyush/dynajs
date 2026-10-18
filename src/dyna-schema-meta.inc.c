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
