/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:netip
/** dyna:netip — IP addresses and CIDR prefixes: parsing, RFC 5952 canonical text, classification and containment. Pure functions, no IO.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:netip" {
    /** A parsed IP address. */
    interface ParsedAddr {
        readonly is4: boolean;
        readonly is6: boolean;
        readonly bytes: Uint8Array;
        readonly string: string;
    }
    /** A parsed CIDR prefix. */
    interface ParsedPrefix {
        readonly addr: string;
        readonly bits: number;
    }
    /** Parses an IPv4/IPv6 address (a 4-in-6 form parses as IPv6); throws TypeError on a malformed address. */
    function parseAddr(addr: string): ParsedAddr;
    /** Parses "addr/bits" into {addr, bits}; the addr is formatted canonically; throws TypeError on a malformed prefix. */
    function parsePrefix(cidr: string): ParsedPrefix;
    /** True when the prefix contains the address; throws TypeError when either argument is malformed. */
    function contains(prefix: string, addr: string): boolean;
    /** The prefix's network address (host bits zeroed), canonically formatted; throws TypeError on a malformed prefix. */
    function masked(cidr: string): string;
    /** The RFC 5952 canonical text of an address; a 4-in-6 address formats as "::ffff:a.b.c.d". */
    function canonical(addr: string): string;
    /** True when the string parses as an IP address; never throws. */
    function isValid(addr: string): boolean;
    /** Total order on addresses: -1, 0, or 1; IPv4 sorts before IPv6. */
    function compareAddr(a: string, b: string): -1 | 0 | 1;
    /** True for 127.0.0.0/8 and ::1. */
    function isLoopback(addr: string): boolean;
    /** True for RFC 1918 space and fc00::/7. */
    function isPrivate(addr: string): boolean;
    /** True for 224.0.0.0/4 and ff00::/8. */
    function isMulticast(addr: string): boolean;
    /** True for 0.0.0.0 and ::. */
    function isUnspecified(addr: string): boolean;
    /** True for 169.254.0.0/16 and fe80::/10. */
    function isLinkLocalUnicast(addr: string): boolean;
    /** True for a global unicast address (not loopback, multicast, link-local or unspecified). */
    function isGlobalUnicast(addr: string): boolean;
    /** True for 224.0.0.0/24 and ff02::/16. */
    function isLinkLocalMulticast(addr: string): boolean;

    /** A compiled CIDR prefix (contains/overlaps/masked/bits), parsed and masked once; unrelated to dyna:structures' string Trie. */
    class Prefix {
        constructor(cidr: string);
        /** True when the address is inside the prefix; an unparseable address is false, not an error. */
        contains(addr: string): boolean;
        /** True when the two prefixes share any address; different families never overlap. */
        overlaps(other: Prefix): boolean;
        /** The network address, canonically formatted. */
        readonly masked: string;
        /** The prefix length. */
        readonly bits: number;
        /** True when this is an IPv4 prefix. */
        readonly isIPv4: boolean;
    }

}
