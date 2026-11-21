//@dts-block @prelude
/** DynaJS types: every `dyna:*` module, the web globals, `std`/`os` (with --std) and the prototype extensions (removed by --no-prototypes).
 *  Option bags are strict: an unknown key throws TypeError. Untrusted input is capped and refused, never truncated. There is no `Intl`. */

// ---- Shared helper types ----

/** A byte-addressed view: a typed array of 1-byte elements, a DataView, or an ArrayBuffer. */
type ByteView = Uint8Array | Int8Array | Uint8ClampedArray | DataView | ArrayBuffer;

/** Bytes input: a string (encoded as UTF-8) or a byte view. BORROW RULE: the buffer is on loan for the call; resize/transfer from a hook mid-call throws TypeError. */
type BytesInput = string | ByteView;

/** 4-byte typed arrays the f32 SIMD kernels accept; Int32Array/Uint32Array are BIT-CAST (raw 32-bit patterns read as IEEE floats), never converted. */
type F32Like = Float32Array | Int32Array | Uint32Array;

/** Any typed array, all element widths; DataView and ArrayBuffer are NOT included (byte-filling natives such as Random.fill refuse them). */
type AnyTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

/** Integer typed arrays: the only kinds the Atomics read-modify-write ops accept (BigInt kinds take/return BigInt). */
type AnyIntegerTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | BigInt64Array | BigUint64Array;

/** Decimal rounding modes (IEEE 754-2008 decimal128 names). */
type RoundingMode = "up" | "down" | "ceil" | "floor" | "halfUp" | "halfDown" | "halfEven" | "halfOdd";

/** Segment-tree fold operations. */
type SegOp = "sum" | "min" | "max";

/** A native resource: released explicitly or by the GC finalizer. */
interface DynResource {
    close(): void;
    dispose(): void;
    readonly closed: boolean;
    readonly [Symbol.dispose]: () => void;
}
