/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:json
/** dyna:json — JSON editing: Pointer (RFC 6901) addressing, Patch (RFC 6902) with copy-on-write, buffered NDJSON. */
declare module "dyna:json" {
    // Neighbours: stable stringify (RFC 8785), JSON5 and JSONPath live in dyna:encoding; streaming NDJSON is dyna:stream's ndjson().

    /** RFC 6901 JSON Pointer. */
    namespace Pointer {
        /** Walks pointer; missing members and out-of-range indices throw. */
        function get<T = unknown>(doc: unknown, pointer: string): T;
        /** Same walk; a missing target returns false instead of throwing. */
        function has(doc: unknown, pointer: string): boolean;
        /** Mutates doc in place (RFC 6902 add semantics) and returns it. */
        function set<T>(doc: T, pointer: string, value: unknown): T;
        /** Mutates doc in place and returns it. */
        function remove<T>(doc: T, pointer: string): T;
        /** ~ -> ~0 and / -> ~1. */
        function escape(token: string): string;
        /** Reverses escape; throws on a `~` not followed by 0 or 1. */
        function unescape(token: string): string;
    }

    /** RFC 6902 JSON Patch. */
    const Patch: {
        /** Runs the six RFC 6902 ops on a PRIVATE deep copy of plain data; non-plain values (Date/RegExp/Map/TypedArray) are shared by reference and the input is never written. */
        apply(doc: unknown, ops: { op: string; path: string; value?: unknown; from?: string }[]): unknown;
    };

    /** NDJSON, buffer-at-once (the streaming form is dyna:stream's ndjson); lines split on LF with one trailing CR stripped; blank lines are skipped. */
    namespace Ndjson {
        /** Parses every non-blank line; a malformed one throws SyntaxError naming its 1-based line number in the text. */
        function parse(text: string): unknown[];
        /** One compact JSON document per line with a trailing newline; an item with no JSON form throws TypeError naming its index. */
        function stringify(items: unknown[]): string;
    }
}
