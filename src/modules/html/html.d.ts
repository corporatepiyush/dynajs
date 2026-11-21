/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:html
/** dyna:html — server-side web content: an HTML5 parser into a shared element tree, CSS-selector queries, an allow-list sanitizer, markdown, templates. */
declare module "dyna:html" {
    /** An element node, the SHARED tree shape: HTMLParse produces it, dyna:scrape's Extractor and Crawl consume it, dyna:xml uses the same layout. */
    interface HTMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | HTMLElement)[];
    }

    /** Parses HTML into an array of root nodes. */
    function HTMLParse(text: string): HTMLElement[];
    /** Serializes a node back to HTML. */
    function HTMLStringify(node: HTMLElement): string;
    /** Extracts the visible text of a node or node array. */
    function HTMLText(node: HTMLElement | HTMLElement[]): string;
    /** Renders Markdown to HTML; allowRawHTML defaults FALSE (raw HTML is escaped; true is the trusted-input opt-in).
     *  headingIds (default false) gives every heading an id slugged from its rendered text: tags and character
     *  references dropped, ASCII lowercased, each space -> "-", letters/digits/"-"/"_"/non-ASCII kept, other ASCII
     *  dropped; a repeat gets "-1", "-2"... (github-slugger's rule); an empty slug gets no id. */
    function MarkdownToHTML(text: string, opts?: { allowRawHTML?: boolean; headingIds?: boolean }): string;

    /** A compiled CSS selector over the parsed tree. */
    class Selector {
        constructor(text: string);
        /** Every matching element. */
        all(doc: HTMLElement | HTMLElement[]): HTMLElement[];
        /** The first match, or undefined when none. */
        first(doc: HTMLElement | HTMLElement[]): HTMLElement | undefined;
        /** True when the single node matches (no combinators allowed). */
        matches(node: HTMLElement): boolean;
    }

    /** Allow-list sanitizer; `protocols` maps "tag.attr" to permitted URL schemes. Output is re-escaped, so re-parsing the output introduces no new structure. */
    class Sanitizer {
        /** An allow-list is required; there is no default policy. */
        constructor(opts: { allow: Record<string, unknown>; protocols?: Record<string, string[]> });
        /** Returns the sanitized HTML. */
        clean(html: string): string;
    }

    /** Rewrites URL-bearing attributes (href, src, srcset candidates, ...) IN PLACE through fn(url, tag, attr); null/undefined keeps the original; returns the count replaced. */
    function rewriteLinks(doc: HTMLElement | HTMLElement[], fn: (url: string, tag: string, attr: string) => unknown): number;

    /** A compiled template; `escape` defaults to TRUE ({escape:false} is the trusted-input opt-in). Caps: 65536 tags, sections 64 deep. */
    class Template {
        constructor(source: string, opts?: { escape?: boolean });
        /** Renders with the data scope; at most 1048576 section elements per render (RangeError, not a hang). */
        render(data?: unknown): string;
    }
}
