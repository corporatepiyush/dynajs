/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:csv
/** dyna:csv — RFC 4180 CSV: in-memory parse/stringify, streaming batches, and CSVFile, a file-backed table.
 *  Cells are VERBATIM: a leading = + - @ can run as a spreadsheet formula; escape it or use DataFrame TO_CSV {escapeFormulas:true}. */
declare module "dyna:csv" {
    /** Parses CSV text in memory; a leading BOM is stripped and empty text is the empty table.
     *  Rows are shaped to the first row's width (short rows pad with "", long rows truncate); rows in syntax errors are 1-based. */
    function parse(text: string, opts?: {
        /** Field separator; exactly one ASCII char, not `"` / CR / LF, != quote. */
        delimiter?: string;
        /** Quote char; exactly one ASCII char, not `,` / CR / LF, != delimiter. */
        quote?: string;
        /** Default true; false parses headerless text: `headers` is [] and every row is data. */
        hasHeader?: boolean;
        /** Default false (tolerant); true throws a SyntaxError naming the row on garbage after a closing quote or an unterminated quote. */
        strict?: boolean;
    }): { headers: string[]; rows: string[][]; totalRows: number };
    /** Serializes rows to CSV text, every line ending in LF. string[][] rows are written verbatim (the header is the caller's first row).
     *  Record rows take their columns from the FIRST row's keys and get a header line unless hasHeader:false; mixing the two forms is refused; an undefined cell or a hole is an empty field, null prints as null, and output past the maximum string length is a RangeError. */
    function stringify(rows: string[][] | Record<string, unknown>[], opts?: {
        delimiter?: string;
        quote?: string;
        /** Default true; false suppresses the derived header of object rows. */
        hasHeader?: boolean;
    }): string;
    /** Options of parseDataFrame and CSVFile#toDataFrame. The first record is the header (names must be unique); a BOM is stripped; a blank line is skipped when the header has 2+ columns. */
    interface DataFrameReadOptions {
        /** Columns to keep, in this order (default: every column, file order); an unknown or repeated name is a TypeError. */
        columns?: string[];
        /** Forces a column's type. Untyped columns are inferred: "f64" when every non-empty cell is a decimal number ([+-]digits[.digits][e[+-]digits], parsed exactly as Number() would; inf, nan, hex and padded cells are text), else "str". An empty cell is NaN in an f64 column and "" in a str column; a forced f64 cell that is not a number, and any i32 cell that is not an integer in int32 range (empty included), is a TypeError naming the row, column and cell. */
        dtypes?: Record<string, "f64" | "i32" | "str">;
        /** Data rows to skip after the header (default 0). */
        offset?: number;
        /** Data rows to keep (default all). */
        limit?: number;
        delimiter?: string;
        quote?: string;
        /** Default false: a short row pads with empty cells, a long row drops its extra cells, text after a closing quote joins the cell, an unterminated quote runs to the end. true makes each of these a SyntaxError naming the record (rows count records from 1 at the header). */
        strict?: boolean;
    }
    /** CSV text straight into a DataFrame in one native pass: numeric columns become Float64Array/Int32Array, text columns dictionary-encoded strings, with no JS string per cell. Empty text is a TypeError (no header); a header-only text is a 0-row frame whose untyped columns are f64.
     *  Regular text (every record the header's width, regular quoting) is read through the structural index of index(), on several threads from 2 MiB; other text through the original scalar reader. Both give the same frame and the same errors. */
    function parseDataFrame(text: string, opts?: DataFrameReadOptions): import("dyna:dataframe").DataFrame;
    /** A structural index over CSV bytes, from index(). Cells are read straight from the source bytes on access; no string exists until one is asked for.
     *  Rows follow parse(): the first record fixes the width (short rows read as "", extra cells are dropped) and, with hasHeader (default true), is the header.
     *  Lifetime: the index pins its source. A string source is held (an ASCII string is read in place); Uint8Array and ArrayBuffer sources are copied at index() time, so later writes to them are not seen; a Path is memory-mapped read-only until the index is collected, and the file must not be truncated meanwhile. */
    interface CsvIndex {
        /** Header cells, or [] with hasHeader:false. */
        readonly headers: string[];
        /** Data rows (the header row excluded). */
        readonly rowCount: number;
        /** The width fixed by the first record. */
        readonly columnCount: number;
        /** One cell; `column` is an index or a header name (TypeError when unknown); a row outside 0..rowCount-1 is a RangeError. */
        get(row: number, column: number | string): string;
        /** One row, padded or truncated to columnCount. */
        row(row: number): string[];
        /** One whole column. No dtype: string[]. "f64": Float64Array parsed exactly as Number() would (decimal text only, an empty cell is NaN). "i32" / "i64": Int32Array / BigInt64Array of decimal integers.
         *  "dict": {codes, values}, values holding each distinct cell once in first-appearance order and codes[i] indexing it, so a group-by needs no per-row string.
         *  A cell that does not fit the dtype is a TypeError naming its row, column and text. Columns of 128 Ki rows or more are converted on several threads. */
        column(column: number | string): string[];
        column(column: number | string, dtype: "f64"): Float64Array;
        column(column: number | string, dtype: "i32"): Int32Array;
        column(column: number | string, dtype: "i64"): BigInt64Array;
        column(column: number | string, dtype: "dict"): { codes: Int32Array; values: string[] };
    }
    /** Indexes CSV text, bytes or a file without creating a string per cell: field boundaries go into one flat offset array, built with SIMD block classification and, for inputs of 2 MiB or more, on several threads.
     *  The dialect, BOM stripping and tolerant/strict rules are parse()'s; strict:true throws the same SyntaxError. Sources up to 4 GiB (RangeError past that). A Path source is memory-mapped: truncating the file from another process while a read or edit is running can terminate the process with SIGBUS; copy the file first if other writers may shrink it. */
    function index(source: string | Uint8Array | ArrayBuffer | import("dyna:file").Path, opts?: {
        delimiter?: string;
        quote?: string;
        /** Default true. */
        hasHeader?: boolean;
        /** Default false. */
        strict?: boolean;
    }): CsvIndex;
    /** A file-backed table: each method load-modify-stores the bound file; writes are atomic whole-file replacements. A Path source is memory-mapped: truncating the file from another process while a read or edit is running can terminate the process with SIGBUS; copy the file first if other writers may shrink it. */
    class CSVFile implements DynResource {
        /** Binds the path; touches no disk until a method runs. */
        constructor(path: import("dyna:file").Path);
        /** Creates the file from headers and optional rows; every header must be a string (a hole included, TypeError naming the index). */
        create(opts: { headers: string[]; rows?: (string | number)[][]; overwrite?: boolean }): { path: import("dyna:file").Path; rows: number };
        /** Loads the file; options offset/limit/columns (columns: strings only, like create's headers). delimiter/quote change the input dialect (read-only support: mutators rewrite the canonical comma/quote pair). */
        read(opts?: { offset?: number; limit?: number; columns?: string[]; strict?: boolean; delimiter?: string; quote?: string }): { headers: string[]; rows: string[][]; totalRows: number };
        /** Appends rows ({rows}: positional arrays or objects keyed by header); a bare array is ONE positional row, truncated to the header's width (only those cells are read). */
        addRow(opts: { rows: (string | number)[][] | Record<string, unknown>[]; strict?: boolean; durable?: boolean } | (string | number)[]): { added: number; totalRows: number };
        /** Sets one cell. */
        updateCell(opts: { row: number; column?: string; columnIndex?: number; value: string; strict?: boolean; durable?: boolean }): { row: number; column: string; value: string };
        /** Removes the data row at `row`; `removed` echoes the removed row's index. */
        removeRow(opts: { row: number; strict?: boolean; durable?: boolean }): { removed: number; totalRows: number };
        /** Appends a column, filling rows with defaultValue. */
        addColumn(opts: { column: string; defaultValue?: string; strict?: boolean; durable?: boolean }): { column: string; totalColumns: number };
        /** Drops the named or indexed column. */
        removeColumn(opts: { column?: string; columnIndex?: number; strict?: boolean; durable?: boolean }): { removedIndex: number; totalColumns: number };
        /** Renames a column; a no-op when names match. */
        renameColumn(opts: { oldName: string; newName: string; strict?: boolean; durable?: boolean }): { oldName: string; newName: string };
        /** One column's values over a [start, end) window -- or its {offset, limit} alias (never both forms); the default 1000-row window cap is raised with maxRows. */
        readColumnValuesRange(opts: { column: string; start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): (string | number)[];
        /** Rows [start, end) as arrays; default is a single row. Accepts {offset, limit} instead of {start, end} (never both); the default 100-row cap is raised with maxRows. */
        readRowRange(opts?: { start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): { headers: string[]; rows: string[][] };
        /** Projects columns over a [start, end) window, capped at 100 rows ({offset, limit} alias accepted, never both forms; maxRows raises the cap). */
        selectColumnRange(opts: { columns: string[]; start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): { columns: string[]; rows: string[][] };
        /** Async-iterable batches {headers, rows, offset, totalRows} of at most `batch` rows (default 1000, max 100000); the file is parsed once, at the first next(), and later batches are cut from that snapshot (a change to the file mid-iteration is not seen). */
        rows(opts?: { batch?: number; delimiter?: string; quote?: string; strict?: boolean }): AsyncIterableIterator<{ headers: string[]; rows: string[][]; offset: number; totalRows: number }>;
        /** Reads the file into a DataFrame with parseDataFrame's options and rules. */
        toDataFrame(opts?: DataFrameReadOptions): import("dyna:dataframe").DataFrame;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
