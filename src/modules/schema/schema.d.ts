/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:schema
/** dyna:schema — JSON Schema (Draft 2020-12) validation: a schema compiles once into a native tree, validation is pure dispatch. */
declare module "dyna:schema" {
    /** JSON Schema Draft 2020-12 validation; a referenced resource declaring an older draft's $schema is still processed under 2020-12 rules. */
    interface SchemaError {
        path: string;
        message: string;
        keyword: string;
    }
    /** The outcome of validate(); `errors` lists the failed keywords. */
    interface SchemaResult {
        valid: boolean;
        errors: SchemaError[];
    }
    /** A compiled schema: reusable, thread-safe, pure dispatch at validate time. */
    interface CompiledSchema {
        /** Validates an instance against the compiled schema. */
        validate(instance: unknown): SchemaResult;
    }
    const Schema: {
        /** Compiles once (Draft 2020-12): $id base URIs, $anchor, $dynamicRef dynamic scope, the embedded 2020-12 meta-schemas, $vocabulary from a meta-schema in resources.
         *  resources maps absolute URI -> schema (nothing is fetched); formats: "registered" (default: only registerFormat names assert), "annotation" or "assertion" (built-ins too). */
        compile(schema: unknown, opts?: { resources?: Record<string, unknown>; baseUri?: string; formats?: "registered" | "annotation" | "assertion" }): CompiledSchema;
        /** Compiles and caches on the schema object; accepts an already-compiled schema. `pattern` and `patternProperties` run under their own budget of 100M backtracking steps plus 1000 per subject character (RegExp itself has none), so a catastrophic pattern from an untrusted schema throws RangeError("dyna:schema: pattern execution timed out") instead of hanging. */
        validate(schema: unknown | CompiledSchema, instance: unknown): SchemaResult;
        /** Registers a custom "format" validator for string instances, live for already-compiled schemas; per runtime; duplicate names are refused. */
        registerFormat(name: string, fn: (value: string) => unknown): void;
    };
}
