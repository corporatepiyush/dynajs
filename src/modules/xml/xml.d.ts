/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:xml
/** dyna:xml — XML three ways: a tree (XMLParse), streaming SAX (push or pull), and a streaming writer; size caps are on and no DTD is processed. */
declare module "dyna:xml" {
    /** A document-tree element node. */
    interface XMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | XMLElement)[];
    }

    /** Parses a document into a tree; `multiple: true` returns every root element. At most one DOCTYPE, before the root; only the five predefined entities exist.
     *  Limits: 256 MiB input (RangeError), 256 open elements (SyntaxError), 16 MiB per token. */
    function XMLParse(text: string, opts?: { trim?: boolean; entities?: "strict" | "keep"; multiple?: boolean }): XMLElement | XMLElement[];
    /** Serializes a node; indent 0..16 spaces; nesting beyond 256 throws. The options bag is strict (unknown key throws). */
    function XMLStringify(node: XMLElement, opts?: { indent?: number }): string;
    /** Collapses an element into a plain object keyed by element name. */
    function XMLToObject(node: XMLElement): Record<string, unknown>;

    /** A streaming document writer for output too deep or long for a tree: only the open-element stack is held; nesting to 65536. */
    class XmlWriter {
        constructor(opts?: { indent?: number });
        /** Writes `<name attrs>` (attribute values escaped) and pushes the element; invalid names throw. */
        open(name: string, attrs?: Record<string, string>): void;
        /** Appends escaped text (& < >). */
        text(s: string): void;
        /** Writes `</name>` for the innermost open element; throws when none is open. */
        close(): void;
        /** The bytes so far, WITHOUT closing anything. */
        toString(): string;
        /** Closes every open element and returns the complete document; idempotent. */
        finish(): string;
    }

    /** Streaming SAX handlers. */
    interface SAXHandlers {
        onOpen?: (name: string, attrs: Record<string, string>) => void;
        onClose?: (name: string) => void;
        onText?: (text: string) => void;
        onCData?: (text: string) => void;
        onComment?: (text: string) => void;
        onPI?: (target: string, data: string) => void;
    }
    /** One pull event: `name` on open/close/pi, `attrs` on open, `text` on text/cdata/comment/pi; `offset` is the absolute byte offset in the stream. A self-closing `<b/>` yields open then close, in push and pull form. */
    interface SAXEvent {
        event: "open" | "close" | "text" | "cdata" | "comment" | "pi";
        name?: string;
        attrs?: Record<string, string>;
        text?: string;
        offset: number;
    }
    /** Streaming SAX parser with XMLParse's document rules; total input is capped at 256 MiB (RangeError), the XMLParse limit.
     *  MODE: with a handlers object it is a PUSH parser (write() fires handlers); with none a PULL parser (write() buffers, next() returns one SAXEvent or null). */
    class SAXParser {
        /** The handlers object is also the options bag (`trim`, `entities`); omit it for the pull form, whose options come from the second argument. */
        constructor(handlers?: SAXHandlers & { trim?: boolean; entities?: "strict" | "keep" }, opts?: { entities?: "strict" | "keep" });
        /** Feeds a string or any byte view. */
        write(chunk: BytesInput): void;
        /** Finalizes the stream. Push parsers run their end-of-document checks here (trailing content throws); pull parsers defer them to the last next(). */
        end(): void;
        /** Pull form: scans forward and returns the next SAXEvent, or null while the input is drained (feed more, or the document is finished). Errors throw and are sticky. */
        next(): SAXEvent | null;
        /** Async-iterable wrapper: drives a pull parser over any sync/async iterable of chunks, yielding one SAXEvent per step. */
        static iterate(source: AsyncIterable<BytesInput> | Iterable<BytesInput>, opts?: { entities?: "strict" | "keep" }): AsyncGenerator<SAXEvent>;
    }
}
