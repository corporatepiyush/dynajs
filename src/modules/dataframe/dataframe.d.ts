/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:dataframe
/** dyna:dataframe — in-memory columnar analytics over TypedArrays: UPPERCASE verbs for filter masks, sort, group-by, joins, windows, quantiles, CSV out.
 *  Masks are Uint8Arrays produced by predicate verbs and accepted by aggregates; string columns are dictionary-encoded. */
declare module "dyna:dataframe" {
    /** One column's description. Numeric text uses `.` regardless of the host locale; POSIX GCC/Clang only (MSVC is not a verified target). */
    interface DataFrameColumn {
        name: string;
        type: string;
    }
    /** A grouped aggregate: keys[i] pairs with values[i]. */
    interface GroupResult {
        keys: (string | number)[];
        values: Float64Array;
    }
    /** GROUP_BY_AGG result: `keys` plus one Float64Array per requested aggregate, always in the order sum, mean, count, min, max; an aggregate not requested is absent. */
    interface GroupAggResult {
        keys: (string | number)[];
        sum?: Float64Array;
        mean?: Float64Array;
        count?: Float64Array;
        min?: Float64Array;
        max?: Float64Array;
    }
    /** Grouped arrays: keys[i] pairs with the Float64Array values[i]. */
    interface GroupArrays {
        keys: (string | number)[];
        values: Float64Array[];
    }
    /** A frame instance: shape getters plus the UPPERCASE verbs. Every verb whose result depends on how equal keys or timestamps are ordered uses one specified order at every row count, so a kernel switching algorithm at a size threshold never changes a result, not even the last bit of a float sum: ties keep input row order unless the verb states its own rule (GROUP_ARRAY_SORTED puts -0 before 0; RANGE_AGG reports -0 bounds as 0). */
    interface DataFrame {
        /** Row count. */
        readonly ROWS: number;
        /** Column count. */
        readonly COLS: number;
        /** Column names in column order. */
        readonly COLUMNS: string[];
        /** Column name to a short type tag: f64 f32 i32 u32 i16 u16 i8 u8 str. */
        DTYPES(): Record<string, string>;
        /** One {name, type} entry per column, in order. */
        SCHEMA(): DataFrameColumn[];
        /** {rows, cols, dtypes, bytes, total_bytes}. */
        INFO(): { rows: number; cols: number; dtypes: Record<string, string>; bytes: Record<string, number>; total_bytes: number };
        /** {columns: {name: bytes}, total}. */
        MEMORY_USAGE(): { columns: Record<string, number>; total: number };
        /** Column name to a fresh TypedArray copy or array of strings. */
        TO_COLUMNS(): Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | string[]>;
        /** One object per row. */
        TO_RECORDS(): Record<string, number | string>[];
        /** The TO_RECORDS array serialised; NaN and Infinity become null. */
        TO_JSON(): string;
        /** CSV text with a header row (RFC 4180 quoting); {escapeFormulas: true} prefixes `'` to cells starting with = + - @ (CSV injection); NaN prints as an empty cell. */
        TO_CSV(opts?: { escapeFormulas?: boolean }): string;
        /** Builds a frame from an array of row objects. */
        FROM_RECORDS(rows: Record<string, unknown>[]): DataFrame;
        /** A fresh frame whose columns are exact copies. */
        COPY(): DataFrame;
        /** The named columns in the order given. */
        SELECT(names: string[]): DataFrame;
        /** The complement, in column order. */
        DROP_COLUMNS(names: string[]): DataFrame;
        /** Renames columns per {old: new}. */
        RENAME(map: Record<string, string>): DataFrame;
        /** The rows where the ROWS-byte mask is nonzero. */
        FILTER(mask: Uint8Array): DataFrame;
        /** Rows [start, end), clamped and negative-indexed like Array.prototype.slice. */
        SLICE(start: number, end?: number): DataFrame;
        /** n rows without replacement via a Fisher-Yates partial shuffle. */
        SAMPLE(n: number, seed?: number): DataFrame;
        /** 1 where the column's value is in values; each value is coerced to the column's type (its error propagates); a hole or undefined entry is a TypeError naming its index. */
        ISIN(col: string, values: (number | string)[]): Uint8Array;
        /** The same shape; rows where mask is 0 become fill. */
        MASK(mask: Uint8Array, fill?: number | string): DataFrame;

        /** Reductions over (col[, mask]); masked-out rows do not contribute. */
        SUM(col: string, mask?: Uint8Array): number;
        /** Aggregates take a column name and an optional row mask (a Uint8Array from the predicate verbs). */
        MIN(col: string, mask?: Uint8Array): number | undefined;
        /** The maximum, ignoring NaN. */
        MAX(col: string, mask?: Uint8Array): number | undefined;
        /** The sum over the count. NaN on an empty selection. */
        MEAN(col: string, mask?: Uint8Array): number;
        /** The number of rows, or of nonzero mask bytes. */
        COUNT(col: string, mask?: Uint8Array): number;
        /** The product, accumulated in double for every column type. */
        PRODUCT(col: string, mask?: Uint8Array): number;
        /** The sum of `a[i] * b[i]` over the selection, via specialised same-type kernels and block-widened mixed pairs. */
        DOT_PRODUCT(a: string, b: string, mask?: Uint8Array): number;
        /** Sample (n-1) variance and standard deviation; the *_POP forms divide by n. */
        VARIANCE(col: string, mask?: Uint8Array): number;
        /** The sample standard deviation. */
        STDDEV(col: string, mask?: Uint8Array): number;
        /** The population variance (/n), defined from one row up. */
        VARIANCE_POP(col: string, mask?: Uint8Array): number;
        /** The population standard deviation. */
        STDDEV_POP(col: string, mask?: Uint8Array): number;
        /** The population skewness. */
        SKEW(col: string, mask?: Uint8Array): number;
        /** The population excess kurtosis. */
        KURTOSIS(col: string, mask?: Uint8Array): number;
        /** The sample skewness. 0 below n=3. */
        SKEW_SAMP(col: string, mask?: Uint8Array): number;
        /** The sample excess kurtosis. 0 below n=4. */
        KURT_SAMP(col: string, mask?: Uint8Array): number;
        /** Standard error of the mean. */
        SEM(col: string, mask?: Uint8Array): number;
        /** The number of NaN values in the selection. */
        COUNT_NULLS(col: string, mask?: Uint8Array): number;
        /** The sum of `w*x` over sum of `w`. A zero weight contributes nothing (it is not in the input set), and all-zero weights give NaN. */
        MEAN_WEIGHTED(valueCol: string, weightCol: string, mask?: Uint8Array): number;
        /** Exact integer sum over integer columns only (TypeError on float columns); RangeError when the total overflows a Number. */
        SUM_CHECKED(col: string, mask?: Uint8Array): number;
        /** {count, sum, mean, min, max, variance, stddev, skew, kurtosis} in one pass. */
        DESCRIBE(col: string, mask?: Uint8Array): { count: number; sum: number; mean: number; min: number; max: number; variance: number; stddev: number; skew: number; kurtosis: number };
        /** Shannon entropy in bits over the empirical value distribution. */
        ENTROPY(col: string, mask?: Uint8Array): number;
        /** The mean absolute deviation from the mean. `undefined` when nothing is selected. */
        MAD(col: string, mask?: Uint8Array): number | undefined;
        /** The median of `|x - median|`. `undefined` when nothing is selected. */
        MEDIAN_ABSOLUTE_DEVIATION(col: string, mask?: Uint8Array): number | undefined;

        /** Bitwise folds over integer columns; empty-selection identities as documented. */
        BITWISE_AND(col: string, mask?: Uint8Array): number;
        /** The bitwise OR of the selection. Identity 0. */
        BITWISE_OR(col: string, mask?: Uint8Array): number;
        /** The bitwise XOR of the selection. Identity 0. */
        BITWISE_XOR(col: string, mask?: Uint8Array): number;
        /** Each group's integer values fold as 32-bit patterns (ToUint32); the result is the unsigned value 0..4294967295. */
        GROUP_BIT_AND(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group bitwise OR of the integer value column. */
        GROUP_BIT_OR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group bitwise XOR of the integer value column. */
        GROUP_BIT_XOR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The count of DISTINCT non-negative integer values, one bit per value. */
        GROUP_BITMAP(col: string, mask?: Uint8Array): number;

        /** Positional access; n defaults to 5 and is clamped to the frame. */
        HEAD(col: string, n?: number, mask?: Uint8Array): Float64Array;
        /** The last `n` selected values, with the same clamping. */
        TAIL(col: string, n?: number, mask?: Uint8Array): Float64Array;
        /** The value of the first SELECTED row. `undefined` when nothing is selected (never NaN, so it cannot be confused with a NaN value). */
        FIRST(col: string, mask?: Uint8Array): number | undefined;
        /** The value of the last selected row. */
        LAST(col: string, mask?: Uint8Array): number | undefined;
        /** The row index of the minimum. Ties go to the FIRST occurrence, NaN values are skipped. */
        ARG_MIN(col: string, mask?: Uint8Array): number | undefined;
        /** The row index of the maximum. Ties go to the first occurrence. */
        ARG_MAX(col: string, mask?: Uint8Array): number | undefined;

        /** Mask-producing comparisons over a column. */
        GT(col: string, value: number): Uint8Array;
        /** Comparison verbs return a row mask (1 = match). */
        GE(col: string, value: number): Uint8Array;
        /** A mask that is 1 where `col[i] < value`. */
        LT(col: string, value: number): Uint8Array;
        /** A mask that is 1 where `col[i] <= value`. */
        LE(col: string, value: number): Uint8Array;
        /** Numeric equality mask; a string column throws TypeError — match strings with ISIN. */
        EQ(col: string, value: number): Uint8Array;
        /** Numeric inequality mask; same string-column refusal as EQ. */
        NE(col: string, value: number): Uint8Array;
        /** 1 where lo <= col[i] <= hi, inclusive at both ends. */
        BETWEEN(col: string, lo: number, hi: number): Uint8Array;
        /** Mask of NaN cells (NOT_NA is its complement). */
        IS_NA(col: string): Uint8Array;
        /** A mask that is 1 where the value is not NaN. */
        NOT_NA(col: string): Uint8Array;
        /** True when every mask byte is nonzero. */
        ALL(mask: Uint8Array): boolean;
        /** True when any mask byte is set. */
        ANY(mask: Uint8Array): boolean;
        /** The mask packed into ceil(ROWS/32) words, LSB first. */
        BITMASK(mask: Uint8Array): Uint32Array;
        /** The AND of the JS truthiness of the stored values (a float NaN is false, any nonzero int is true). */
        BOOL_AND(col: string, mask?: Uint8Array): boolean;
        /** The OR of the JS truthiness of the stored values. Vacuous over zero selected rows: false. */
        BOOL_OR(col: string, mask?: Uint8Array): boolean;
        /** The parity of the true count. Vacuous over zero selected rows: false. */
        BOOL_XOR(col: string, mask?: Uint8Array): boolean;
        /** 1 on the FIRST occurrence of each distinct value. */
        DROP_DUPLICATES(col: string, mask?: Uint8Array): Uint8Array;
        /** 1 where none of the named columns is NaN; no arguments: every numeric column. */
        DROP_NA(...cols: string[]): Uint8Array;

        /** Elementwise verbs return a Float64Array of ROWS entries; binary ones take a number or another COLUMN NAME (RSUB/RDIV: number only). With { out: name } (here and on CLIP, FILL_NA, ROLLING_*, EMA) the result is written into that column and the frame is returned: an existing out column must be a Float64Array column; a name the frame lacks adds a new f64 column after the others (MUL("price", "qty", { out: "rev" })), up to the 1024-column cap, and a verb that throws adds nothing. */
        ABS(col: string): Float64Array;
        ABS(col: string, opts: { out: string }): this;
        /** Each element rounded half away from zero. */
        ROUND(col: string): Float64Array;
        ROUND(col: string, opts: { out: string }): this;
        /** The largest integer <= each value. */
        FLOOR(col: string): Float64Array;
        FLOOR(col: string, opts: { out: string }): this;
        /** The smallest integer >= each value. */
        CEIL(col: string): Float64Array;
        CEIL(col: string, opts: { out: string }): this;
        /** The square root of each element. */
        SQRT(col: string): Float64Array;
        SQRT(col: string, opts: { out: string }): this;
        /** The natural logarithm of each element. */
        LOG(col: string): Float64Array;
        LOG(col: string, opts: { out: string }): this;
        /** E raised to each value. */
        EXP(col: string): Float64Array;
        EXP(col: string, opts: { out: string }): this;
        /** -1, 0 or 1 per element. */
        SIGN(col: string): Float64Array;
        SIGN(col: string, opts: { out: string }): this;
        /** Each element clamped into `[lo, hi]`. NaN elements pass through unchanged. */
        CLIP(col: string, lo: number, hi: number): Float64Array;
        CLIP(col: string, lo: number, hi: number, opts: { out: string }): this;
        /** The column with NaN replaced by `value`. A NaN `value` is legal and is a no-op. */
        FILL_NA(col: string, value: number): Float64Array;
        FILL_NA(col: string, value: number, opts: { out: string }): this;
        /** Elementwise sum; a string operand is a column REFERENCE, so ADD("a", "b") adds two columns. */
        ADD(col: string, x: number | string): Float64Array;
        ADD(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise difference. */
        SUB(col: string, x: number | string): Float64Array;
        SUB(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise product. */
        MUL(col: string, x: number | string): Float64Array;
        MUL(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise quotient. */
        DIV(col: string, x: number | string): Float64Array;
        DIV(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise power. */
        POW(col: string, x: number | string): Float64Array;
        POW(col: string, x: number | string, opts: { out: string }): this;
        /** k - col; number-only operand. */
        RSUB(col: string, k: number): Float64Array;
        RSUB(col: string, k: number, opts: { out: string }): this;
        /** k / col; number-only operand. */
        RDIV(col: string, k: number): Float64Array;
        RDIV(col: string, k: number, opts: { out: string }): this;
        /** a where the mask byte is nonzero, else b. */
        WHERE(mask: Uint8Array, a: string | number, b: string | number): Float64Array;

        /** Grouped verbs: integer keys are dense-window encoded in [0, 2^20) (else RangeError) and list every key 0..max, empty ones included; string keys list the strings the rows hold. */
        GROUP_BY_SUM(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group mean. A group with no contributing row is NaN. */
        GROUP_BY_MEAN(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group minimum, ignoring NaN. An empty group, or one whose values are all NaN, is NaN. */
        GROUP_BY_MIN(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group maximum, ignoring NaN. An empty group, or one whose values are all NaN, is NaN. */
        GROUP_BY_MAX(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Rows per group; takes no value column. */
        GROUP_BY_COUNT(key: string, mask?: Uint8Array): GroupResult;
        /**
         * Several grouped aggregates in ONE pass over the rows. `aggs` names any of "sum", "mean", "count", "min", "max", each at most once;
         * an empty array, an unknown or repeated name, or a non-string entry is a TypeError. Each field equals the matching single verb
         * called with the same key, val and mask: sum = GROUP_BY_SUM, mean = GROUP_BY_MEAN, count = GROUP_BY_COUNT, min = GROUP_BY_MIN,
         * max = GROUP_BY_MAX, so keys follow the same rules (dense integer keys, string keys pruned to the strings the rows hold).
         * NaN: sum and mean propagate a NaN value; count counts the row; min and max ignore it, so a group whose values are all NaN
         * has NaN min and max. Empty group (no row passes the mask): sum 0, mean NaN, count 0, min NaN, max NaN. A string value column
         * is a TypeError. Summation order of GROUP_BY_SUM / MEAN / COUNT / MIN / MAX / AGG keyed by an i32 or str column over an f64 value column
         * (or COUNT alone): row order below 2 Mi rows; from 2 Mi rows with at most 65536 groups, each 2^20-row block is summed in row order and the block totals are added in block order (blocks may run on several
         * threads). The order depends only on ROWS and the group count, so results are deterministic on every machine.
         */
        GROUP_BY_AGG(key: string, val: string, aggs: ("sum" | "mean" | "count" | "min" | "max")[], mask?: Uint8Array): GroupAggResult;
        /** GROUP_BY_SUM under the name the map API uses. Identical call and result. */
        SUM_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_MIN by its map name. */
        MIN_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_MAX by its map name. */
        MAX_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Per-key arrays of values; the GROUP_ARRAY_* variants dedupe, sort, window or keep the last k. */
        GROUP_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** GROUP_ARRAY keeping only the first occurrence of each (group, value) pair. */
        GROUP_UNIQ_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** The per-group moving sum. O(m) block decomposition above `w` = 256, O(m*w) re-sum below, one shared threshold. */
        GROUP_ARRAY_MOVING_SUM(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        /** The per-group moving average. Divides by what CONTRIBUTED, never by `w`. */
        GROUP_ARRAY_MOVING_AVG(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        /** Each group's values sorted ascending in Float64Array.prototype.sort order (-0 before 0, NaN last, NaNs in row order) at every group size: radix from 64 values, a shell sort of the order-preserving bit keys below. */
        GROUP_ARRAY_SORTED(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** The last `k` rows seen per group (a circular window, O(n) instead of O(n*k)). */
        GROUP_ARRAY_LAST(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The first k values in ROW order per group -- a deterministic head, not a random sample. */
        GROUP_ARRAY_SAMPLE(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The values present in EVERY group. */
        GROUP_ARRAY_INTERSECT(key: string, val: string, mask?: Uint8Array): Float64Array;
        /** A dense array of size slots; later rows overwrite earlier ones. */
        GROUP_ARRAY_INSERT_AT(value: string, position: string, size: number, fill?: number, mask?: Uint8Array): Float64Array;
        /** The selected values joined in row order with `sep`. String columns join their dictionary strings, numeric columns their numbers. */
        GROUP_CONCAT(col: string, sep?: string, mask?: Uint8Array): string;
        /** One JSON object mapping each key to an array of that group's values. */
        JSON_AGG(key: string, value: string, mask?: Uint8Array): string;
        /** One JSON object mapping each key to the LAST value that appeared. */
        JSON_OBJECT_AGG(key: string, value: string, mask?: Uint8Array): string;
        /** JSON_AGG, but throws a RangeError on any non-finite value instead of serialising null. */
        JSON_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;
        /** JSON_OBJECT_AGG with the same strictness. */
        JSON_OBJECT_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;

        /** Ordering: sorting, ranking, frequency. NaN sorts last. */
        SORT(col: string, mask?: Uint8Array): Float64Array;
        /** Row indices that sort the column ascending. */
        ARG_SORT(col: string, mask?: Uint8Array): Uint32Array;
        /** Average ranks; ties share the mean of their positions. */
        RANK(col: string, mask?: Uint8Array): Float64Array;
        /** Ranks counting distinct values. */
        DENSE_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** (min rank - 1) / (n - 1); a single valued row is 0. NaN rows stay NaN. */
        PERCENT_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** SQL NTILE; first n % buckets tiles take one extra row. */
        NTILE(col: string, buckets: number, mask?: Uint8Array): Float64Array;
        /** The `k` largest values in descending order. A `k` larger than the selection clamps. */
        N_LARGEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** The `k` smallest values in ascending order. */
        N_SMALLEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** Distinct values in first-seen order; strings for a string column. */
        UNIQUE(col: string, mask?: Uint8Array): Float64Array | string[];
        /** The count of distinct values PRESENT in the selection (a string column's dictionary may hold more). */
        N_UNIQUE(col: string, mask?: Uint8Array): number;
        /** Exact distinct count, or n+1 meaning "more than n"; numeric columns only. */
        UNIQ_UP_TO(col: string, n: number, mask?: Uint8Array): number;
        /** Distinct values with their counts (TOP_K keeps the k most frequent). */
        VALUE_COUNTS(col: string, mask?: Uint8Array): GroupResult;
        /** The `k` most FREQUENT values (the frequency question, not the magnitude one). */
        TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** The most frequent value; ties go to the first in row order. */
        MODE(col: string, mask?: Uint8Array): number | string | undefined;
        /** Sketch-based approximate distinct count. */
        APPROX_COUNT_DISTINCT(col: string, mask?: Uint8Array): number;
        /** A Space-Saving estimate of the `k` most frequent values. */
        APPROX_TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** Ranks by summed weight, not count. */
        APPROX_TOP_SUM(col: string, weightCol: string, k: number, mask?: Uint8Array): GroupResult;
        /** The `k` values with the largest summed weight (or frequency when the weight column is omitted). Each key's weights are summed in row order, so the sums are deterministic to the last bit; -0 and 0 are one key, reported as 0. */
        TOP_K_WEIGHTED(col: string, weightCol: string | undefined, k: number, mask?: Uint8Array): GroupResult;
        /** The value holding strictly more than half the total weight. */
        ANY_HEAVY(col: string, weightCol?: string, mask?: Uint8Array): number | undefined;
        /** MinHash Jaccard estimate between two columns. */
        APPROX_SIMILARITY(a: string, b: string, mask?: Uint8Array): number;

        /** Quantiles: select rather than sort. */
        QUANTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Interpolated quantile for q in [0, 1]; PERCENTILE_DISC returns an actual element. */
        PERCENTILE_CONT(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The order statistic at `ceil(q * n)`, so the answer IS a value the column holds. */
        PERCENTILE_DISC(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** QUANTILE at 0.5. Pandas-style NaN for an empty selection. */
        MEDIAN(col: string, mask?: Uint8Array): number | undefined;
        /** The order statistic at `floor(q * (n-1))`, never interpolated. */
        QUANTILE_EXACT_LOW(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The order statistic at `ceil(q * (n-1))`, never interpolated. */
        QUANTILE_EXACT_HIGH(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Many interpolating quantiles from ONE gather. */
        QUANTILES(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** Many approximate quantiles off ONE t-digest. Equal values enter the digest in row order (-0 and 0 tie) at every size, so the result is deterministic to the last bit. */
        QUANTILES_TDIGEST(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** One t-digest quantile. `undefined` for no values. Equal values enter the digest in row order, as in QUANTILES_TDIGEST. */
        APPROX_PERCENTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The value at which cumulative weight first reaches `q` of the total, so a row weighing three counts as three rows. Equal values accumulate their weights in row order; -0 and 0 are one value, reported as 0. */
        QUANTILE_EXACT_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** The approximate form in bounded memory. Equal values enter the digest in row order with their weights (-0 and 0 tie), so the result is deterministic to the last bit. */
        QUANTILE_TDIGEST_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** Equal-width bins over the observed range; edges has bins+1 entries. */
        HISTOGRAM(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };
        /** The same bins with counts divided by the number of contributing rows. */
        HISTOGRAM_NORMALIZED(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };

        /** Scans: windowed and sequential verbs over exactly ROWS entries. */
        CUM_SUM(col: string, mask?: Uint8Array): Float64Array;
        /** The running product (identity 1). */
        CUM_PROD(col: string, mask?: Uint8Array): Float64Array;
        /** The running maximum (identity -Infinity). */
        CUM_MAX(col: string, mask?: Uint8Array): Float64Array;
        /** The running minimum (identity +Infinity). */
        CUM_MIN(col: string, mask?: Uint8Array): Float64Array;
        /** out[i] = col[i - periods]; the vacated head/tail is NaN. */
        SHIFT(col: string, periods?: number): Float64Array;
        /** Difference from the row `periods` earlier (default 1). */
        DIFF(col: string, periods?: number): Float64Array;
        /** Rolling-window verbs over the trailing `w` rows. */
        ROLLING_SUM(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_SUM(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_SUM(col: string, w: number, opts: { out: string }): this;
        /** The trailing window mean. Divides by what CONTRIBUTED, never by `w`. */
        ROLLING_MEAN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MEAN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MEAN(col: string, w: number, opts: { out: string }): this;
        /** The trailing window minimum. Monotonic-deque O(n); NaN never enters the deque. */
        ROLLING_MIN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MIN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MIN(col: string, w: number, opts: { out: string }): this;
        /** The trailing window maximum. Same deque. */
        ROLLING_MAX(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MAX(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MAX(col: string, w: number, opts: { out: string }): this;
        /** The sample variance (ddof=1) per window, two passes, never a sum of squares around an uncentred mean. */
        ROLLING_VAR(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_VAR(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_VAR(col: string, w: number, opts: { out: string }): this;
        /** The sqrt of the rolling variance. */
        ROLLING_STD(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_STD(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_STD(col: string, w: number, opts: { out: string }): this;
        /** Exponential moving average; alpha in (0, 1]. */
        EMA(col: string, alpha: number, mask?: Uint8Array): Float64Array;
        EMA(col: string, alpha: number, mask: Uint8Array, opts: { out: string }): this;
        EMA(col: string, alpha: number, opts: { out: string }): this;
        /** `(x[i] - x[i-p]) / x[i-p]`. A zero previous value gives +/-Inf (the honest answer); 0/0 stays NaN. */
        PCT_CHANGE(col: string, periods?: number, mask?: Uint8Array): Float64Array;
        PCT_CHANGE(col: string, periods: number, mask: Uint8Array, opts: { out: string }): this;
        PCT_CHANGE(col: string, periods: number, opts: { out: string }): this;
        PCT_CHANGE(col: string, opts: { out: string }): this;
        /** `(x - mean) / sample stddev`, so it composes with STDDEV. */
        ZSCORE(col: string, mask?: Uint8Array): Float64Array;
        ZSCORE(col: string, mask: Uint8Array, opts: { out: string }): this;
        ZSCORE(col: string, opts: { out: string }): this;
        /** Sum of positive consecutive differences. */
        DELTA_SUM(col: string, mask?: Uint8Array): number;
        /** DELTA_SUM in timestamp order, not row order. Rows with equal timestamps are taken in row order (a stable sort at every size), so the result is deterministic. */
        DELTA_SUM_TIMESTAMP(valueCol: string, timeCol: string, mask?: Uint8Array): number;

        /** Pairwise statistics; regression verbs take (y, x). */
        COV_POP(a: string, b: string, mask?: Uint8Array): number;
        /** The sample covariance (n-1). NaN below two selected rows. */
        COV_SAMP(a: string, b: string, mask?: Uint8Array): number;
        /** Pearson correlation of two columns; NaN on zero variance. */
        CORR(a: string, b: string, mask?: Uint8Array): number;
        /** Least-squares regression of y on x (the SQL REGR_* family). */
        REGR_SLOPE(y: string, x: string, mask?: Uint8Array): number;
        /** The regression intercept. */
        REGR_INTERCEPT(y: string, x: string, mask?: Uint8Array): number;
        /** The squared correlation, clamped to 1.0. A constant `y` gives NaN (SQL's special case of 1 is not followed). */
        REGR_R2(y: string, x: string, mask?: Uint8Array): number;
        /** The mean of the x column, under the (y, x) naming. */
        REGR_AVG_X(y: string, x: string, mask?: Uint8Array): number;
        /** The mean of the y column. */
        REGR_AVG_Y(y: string, x: string, mask?: Uint8Array): number;
        /** The number of selected rows. */
        REGR_COUNT(x: string, y: string, mask?: Uint8Array): number;
        /** The sum of (x - mean x)^2. Note the argument order: SXX is the INDEPENDENT column's sum of squares. */
        REGR_SXX(y: string, x: string, mask?: Uint8Array): number;
        /** The sum of (y - mean y)^2. */
        REGR_SYY(y: string, x: string, mask?: Uint8Array): number;
        /** The sum of (y - mean y)(x - mean x). */
        REGR_SXY(y: string, x: string, mask?: Uint8Array): number;
        /** Spearman: Pearson over the average ranks. */
        RANK_CORR(x: string, y: string, mask?: Uint8Array): number;
        /** The n*n row-major correlation matrix. */
        CORR_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** The covariance matrix over the same moments as COV_SAMP. */
        COV_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** Change per unit time across the WHOLE selection. */
        RATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The most recent interval only. */
        IRATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The slope joining the leftmost and rightmost points by x value. */
        BOUNDING_RATIO(x: string, y: string, mask?: Uint8Array): number;
        /** Exponentially time-decayed aggregates over a time column with decay constant `tau`. */
        EXPONENTIAL_TIME_DECAYED_AVG(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The weighted sum. */
        EXPONENTIAL_TIME_DECAYED_SUM(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The decayed row count (the weight denominator; the value is ignored). */
        EXPONENTIAL_TIME_DECAYED_COUNT(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The largest weighted value. */
        EXPONENTIAL_TIME_DECAYED_MAX(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** Half-open [lo, hi) ranges merged into their union. -0 and 0 are one bound, reported as 0, so the result does not depend on row order. */
        RANGE_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { starts: Float64Array; ends: Float64Array };
        /** The interval common to ALL ranges. */
        RANGE_INTERSECT_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { start: number; end: number } | undefined;

        /** Reshape; produced frames carry fresh copies of the data. */
        JOIN(other: DataFrame, leftKey: string, rightKey: string, how?: "inner" | "left" | "right" | "outer"): DataFrame;
        /** As-of join: each left row takes the right row with the largest rightTime <= leftTime (NaN fill); both time columns integer and ascending. */
        ASOF_JOIN(other: DataFrame, leftTime: string, rightTime: string): DataFrame;
        /** Appends the rows of another frame with the same columns. */
        CONCAT(other: DataFrame): DataFrame;
        /** Buckets a sorted numeric time column; agg sum|mean|min|max|count. */
        RESAMPLE(timeCol: string, interval: number, agg?: "sum" | "mean" | "min" | "max" | "count"): DataFrame;
        /** One row per distinct index value, one column per distinct key value. */
        PIVOT(index: string, columns: string, values: string, agg?: string): DataFrame;
        /** Long form: each (row, valueVar) pair becomes one output row. */
        MELT(idVars: string[], valueVars: string[]): DataFrame;
    }


    /** Brand check: true only for frames this engine built, never for look-alike objects. */
    function isDataFrame(v: unknown): boolean;
    /** Builds a frame from { name: TypedArray | string[] }. ZERO-COPY: TypedArray columns are ALIASED, not copied; `number[]` columns are stored as STRINGS. */
    const DataFrame: {
        /** A column may also be a dictionary {codes: Int32Array, values: string[]} (the shape of dyna:csv CsvIndex.column(c, "dict")): it becomes a string column with no per-row string; codes are copied, values must be distinct strings (TypeError) and every code must index one (RangeError). */
        new (columns: Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | number[] | string[] | { codes: Int32Array; values: string[] }>): DataFrame;
    };
}