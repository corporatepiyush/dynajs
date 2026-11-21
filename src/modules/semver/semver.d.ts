/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:semver
/** dyna:semver — semver 2.0.0: parse, compare, increment, and ranges with node-semver semantics. */
declare module "dyna:semver" {
    /** A parsed semver 2.0.0 version. */
    interface SemVer {
        major: number;
        minor: number;
        patch: number;
        prerelease: (string | number)[];
        build: string[];
        version: string;
    }

    /** Parses major.minor.patch[-pre][+build]; values above MAX_SAFE throw. */
    function parse(version: string): SemVer;
    /** Whether the string parses at all. */
    function isValid(version: string): boolean;
    /** Trims, strips a leading `=`, returns the normalized version or null. */
    function clean(version: string): string | null;
    /** The first run of digits normalized into X.Y.Z, or null. */
    function coerce(version: string): string | null;
    /** -1 | 0 | 1 by semver precedence (build metadata does not count). */
    function compare(a: string, b: string): -1 | 0 | 1;
    /** Whether the versions are equal. */
    function eq(a: string, b: string): boolean;
    /** Whether the versions differ. */
    function neq(a: string, b: string): boolean;
    /** Whether `a` is greater than `b`. */
    function gt(a: string, b: string): boolean;
    /** Whether `a` is greater than or equal to `b`. */
    function gte(a: string, b: string): boolean;
    /** Whether `a` is less than `b`. */
    function lt(a: string, b: string): boolean;
    /** Whether `a` is less than or equal to `b`. */
    function lte(a: string, b: string): boolean;
    /** Ascending sort by precedence; does not mutate the input. */
    function sort(versions: string[]): string[];
    /** The major field. */
    function major(v: string): number;
    /** The minor field. */
    function minor(v: string): number;
    /** The patch field. */
    function patch(v: string): number;
    /** The prerelease identifiers, or null when there are none. */
    function prerelease(v: string): (string | number)[] | null;
    /** Bumps by major|minor|patch|premajor|preminor|prepatch|prerelease|release; 'release' strips the prerelease and returns null when there is none. */
    function inc(version: string, release: string, identifier?: string): string | null;
    /** One-shot range match; a prerelease matches only ranges naming a prerelease at the same major.minor.patch. */
    function satisfies(version: string, range: string): boolean;
    /** The highest version in the list that satisfies the range, or null. */
    function maxSatisfying(versions: string[], range: string): string | null;
    /** The lowest version in the list that satisfies the range, or null. */
    function minSatisfying(versions: string[], range: string): string | null;
    /** The highest-precedence component that differs ("major" ... "build"), or null when identical. */
    function diff(a: string, b: string): "major" | "minor" | "patch" | "prerelease" | "build" | null;
    /** compare(), with build identifiers breaking a precedence tie (absent build sorts lowest). */
    function compareBuild(a: string, b: string): -1 | 0 | 1;
    /** Whether two ranges (strings or Range objects) intersect, by node-semver's comparator-pairwise rule. */
    function intersects(rangeA: string | Range, rangeB: string | Range): boolean;

    /** A compiled range expression; same prerelease rule as satisfies(); an invalid range is a TypeError. */
    class Range {
        /** A compiled `Range`. */
        constructor(rangeString: string);
        /** Whether the version matches the compiled range. */
        test(version: string): boolean;
        /** The versions that match, preserving input order. */
        filter(versions: string[]): string[];
        /** The highest matching version, or `null`. */
        maxSatisfying(versions: string[]): string | null;
        /** The lowest matching version, or `null`. */
        minSatisfying(versions: string[]): string | null;
        /** Whether this range intersects `other`. */
        intersects(other: string | Range): boolean;
        /** The original range text. */
        readonly source: string;
        /** The number of `||`-separated sets (the observable measure of compilation). */
        readonly setCount: number;
    }
}
