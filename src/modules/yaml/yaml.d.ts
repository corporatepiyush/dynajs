/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:yaml
/** dyna:yaml — YAML 1.2 core schema: Parse, ParseAll, lazy ParseStream, Stringify; anchors and aliases are refused by name, never expanded. */
declare module "dyna:yaml" {
    /** Parses exactly one document. { maxDepth: 1..128 } lowers the nesting cap; { schema } accepts "core" and refuses "full" by name. A leading BOM is skipped and NUL is kept. A core-schema int (decimal, 0x, 0o) is a Number, rounded like JSON.parse beyond +-(2^53-1) (owner decision D8: lossy by default, so 9007199254740993 reads as 9007199254740992); { bigint: true } makes such an int an exact BigInt. A digit run over 4096 characters stays a plain string in both modes. A non-boolean bigint, an unknown key or a non-object options value is a TypeError. */
    function Parse(text: string, opts?: { maxDepth?: number; schema?: "core" | "full"; bigint?: boolean }): unknown;
    /** Parses every `---`-separated document into an array. Same options as Parse. */
    function ParseAll(text: string, opts?: { maxDepth?: number; schema?: "core" | "full"; bigint?: boolean }): unknown[];
    /** Serializes to YAML. { indent } 1..10 (default 2); { width } > 0 folds long strings losslessly (0 = never); { sortKeys } orders keys bytewise; { flow } emits `[a, b]`. Functions follow JSON.stringify: a function-valued property is omitted and a function in an array or at the top is null (function source is never written). A BigInt is a plain integer scalar (`-5n` -> `-5`) and parses back as the same BigInt (with { bigint: true }) up to 4096 characters; a string that reads as an int is quoted. An integer Number beyond +-(2^53-1) gets a `.0` and -0 is `-0.0`, so both parse back as the same Number. */
    function Stringify(value: unknown, opts?: {
        indent?: number;
        width?: number;
        sortKeys?: boolean;
        flow?: boolean;
    }): string;
    /** Lazy multi-document parsing: a SYNC iterator over the `---`-separated documents, one alive at a time; an error names its document and line and ends the iterator. Same options as Parse. */
    function ParseStream(text: string, opts?: { maxDepth?: number; schema?: "core" | "full"; bigint?: boolean }): Iterator<unknown>;
}