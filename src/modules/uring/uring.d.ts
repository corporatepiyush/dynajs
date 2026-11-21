/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:uring
/** dyna:uring — batched disk reads over io_uring; the module exists only in Linux builds with CONFIG_IO_URING=y. */
declare module "dyna:uring" {
    /** Whole file as a string via the io_uring bulk reader (Linux only); no size cap. */
    function readFile(path: import("dyna:file").Path): string;
    /** Whole file as a string via the blocking pread(2) reference reader; no size cap. */
    function readFileSync(path: import("dyna:file").Path): string;
    /** Whole file as a fresh Uint8Array via the io_uring bulk reader; same Path contract as readFile. */
    function readFileBytes(path: import("dyna:file").Path): Uint8Array;
    /** Byte count plus a 32-bit FNV-1a checksum of a whole file; NON-cryptographic (use dyna:hash for integrity). */
    function checksum(path: import("dyna:file").Path, useUring?: boolean): { bytes: number; sum: number };
}
