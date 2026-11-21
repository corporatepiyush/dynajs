/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:asn1
/** dyna:asn1 — ASN.1 DER node builders, encode, bounded decode (nesting <= 256). */
declare module "dyna:asn1" {
    /** Canonical DER codec. */
    interface ASN1Node {
        cls: number;
        tag: number;
        constructed: boolean;
        value: unknown;
    }
    /** ASN.1 DER: node builders, encode and decode over one node tree. */
    const ASN1: {
        /** DER-encodes a node tree. */
        encode(node: ASN1Node): Uint8Array;
        /** Untrusted input: nesting <= 256, children per construct <= 65536, total nodes <= 1048576 (RangeError before any proportional allocation). */
        decode(bytes: Uint8Array | ArrayBuffer): ASN1Node;
        /** A SEQUENCE or SET node over the children. */
        seq(children: ASN1Node[]): ASN1Node;
        /** A SEQUENCE or SET node over the children. */
        set(children: ASN1Node[]): ASN1Node;
        /** Any integer Number or BigInt as minimal two's complement (64 content bytes at most); decodes to a Number up to 2^53, a BigInt beyond. */
        int(value: number | bigint): ASN1Node;
        /** A BOOLEAN node. */
        bool(value: boolean): ASN1Node;
        /** A NULL node. */
        null(): ASN1Node;
        /** An OCTET STRING node. */
        octets(bytes: ByteView): ASN1Node;
        /** BIT STRING with `unused` trailing pad bits. */
        bitString(bytes: ByteView, unused: number): ASN1Node;
        /** An OBJECT IDENTIFIER node. */
        oid(str: string): ASN1Node;
        /** A UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. */
        utf8(str: string): ASN1Node;
        /** IA5String (tag 22): ASCII only; non-ASCII refuses at encode. */
        ia5String(str: string): ASN1Node;
        /** BMPString (tag 30), UCS-2 big-endian; code points above U+FFFF and lone surrogates are refused. */
        bmpString(str: string): ASN1Node;
        /** UniversalString (tag 28), UCS-4 big-endian; lone surrogates are refused. */
        universalString(str: string): ASN1Node;
        /** A UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. */
        printable(str: string): ASN1Node;
        /** A UTCTime or GeneralizedTime node (the strings are emitted verbatim). */
        utcTime(str: string): ASN1Node;
        /** A UTCTime or GeneralizedTime node (the strings are emitted verbatim). */
        generalizedTime(str: string): ASN1Node;
        /** Context-specific primitive [tag] with raw content. */
        context(tag: number, content: ByteView): ASN1Node;
        /** Context-specific constructed [tag] wrapping child nodes. */
        contextC(tag: number, children: ASN1Node[]): ASN1Node;
    };
}
