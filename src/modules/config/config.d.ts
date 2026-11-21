/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:config
/** dyna:config — configuration files: TOML 1.0, INI, `.env` (with optional process-environment assignment), front matter. */
declare module "dyna:config" {
    /** TOML 1.0; date-times stay RFC 3339 strings. SyntaxError: a quote run of six or more, or `[[x]]` extending anything but a previous `[[x]]` header. */
    namespace TOML {
        /** Parses a full TOML 1.0 document; key collisions and leading zeros are refused. A leading BOM is skipped and NUL in strings is kept. An integer is a Number, rounded like JSON.parse beyond +-(2^53-1) (owner decision D8: lossy by default); { bigint: true } returns an exact BigInt beyond +-(2^53-1) instead (TOML integers are int64; wider is a SyntaxError). A non-object options value, an unknown key or a non-boolean bigint is a TypeError. */
        function parse(text: string, opts?: { bigint?: boolean }): Record<string, unknown>;
        /** Serializes a plain object root; NaN/Infinity render as nan/inf/-inf, -0 as -0.0. A BigInt in the int64 range is an integer (wider is a TypeError); an integer Number beyond +-(2^53-1) is written as a float so it parses back as the same Number. `null`/`undefined` are a TypeError (TOML 1.0 has no null literal). */
        function stringify(value: unknown): string;
    }
    /** Classic INI reading and writing. */
    namespace INI {
        /** Reads [section] headers, key=value pairs, key[]=v lists, and bare keys as true. */
        function parse(text: string): Record<string, unknown>;
        /** Object to INI: top-level scalars, then one [section] per object value; values parse back as strings; deeper nesting, arrays and null are refused. */
        function stringify(record: Record<string, unknown>): string;
    }
    /** dotenv grammar .env parsing and loading. */
    namespace Env {
        /** Parses KEY=value records; lines without `=` are skipped. In double-quoted values `\n` `\t` `\r` `\0` expand to LF, TAB, CR, NUL and a backslash before any other character is dropped, so stringify() output (NUL included) round-trips. */
        function parse(text: string): Record<string, string>;
        /** Parses a .env file and assigns it into the process environment. Defaults: assign true, override false, expand false; with expand, values expand in file order.
         *  Caps: file 8 MiB; keys are 1..4096 bytes without `=` or NUL; a NUL in a value is a TypeError, an expansion above 1 MiB a RangeError. */
        function load(path: string, opts?: { assign?: boolean; override?: boolean; expand?: boolean }): Record<string, string>;
        /** Record to .env text; values must be strings, and those that cannot sit bare are double-quoted with the escapes parse() expands. */
        function stringify(record: Record<string, string>): string;
    }
    /** Front-matter splitting (YAML/TOML/JSON fences); data stays text. */
    namespace FrontMatter {
        /** Splits at a first-line fence; `data`/`lang` are null when absent. */
        function split(text: string): { data: string | null; body: string; lang: string | null };
    }
}
