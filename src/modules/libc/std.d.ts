/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block std
/** std — QuickJS-compatible stdio and environment module, present only under --std; failures are errno values, not throws. */
declare module "std" {
    interface StdFile {
        /** Method, not a property (matches the implementation). */
        eof(): boolean;
        /** Method, not a property. */
        error(): boolean;
        getByte(): number;
        putByte(b: number): void;
        readBytes(max: number): Uint8Array;
        readAsString(max: number): string;
        writeBytes(bytes: Uint8Array): void;
        writeStr(str: string): void;
        getline(): string | null;
        fileno(): number;
        close(): void;
        seek(offset: number, whence: number): void;
        tell(): number;
    }
    const __stdin: StdFile;
    const __stdout: StdFile;
    const __stderr: StdFile;
    export { __stdin as in, __stdout as out, __stderr as err };
    /** Format and print to stdout. Width/precision (digits and `*`) capped at 1000000 (RangeError); %c honours width/flags like C. */
    function printf(fmt: string, ...args: unknown[]): void;
    /** Format to a string. Width/precision (digits and `*`) capped at 1000000 (RangeError); %c honours width/flags like C. */
    function sprintf(fmt: string, ...args: unknown[]): string;
    function puts(str: string): void;
    function getenv(name: string): string | undefined;
    function setenv(name: string, value: string): void;
    function unsetenv(name: string): void;
    /** The whole environment as a {name: value} record. */
    function getenviron(): Record<string, string>;
    /** Exit now via exit(3): flushes the dyna:stream stdout()/stderr() buffers first; an unreported write error found by that flush is printed and turns code 0 into 1. */
    function exit(code?: number): void;
    function gc(): void;
    /** Evaluates the given script in global scope. */
    function evalScript(script: string, options?: unknown): unknown;
    /** Loads a script file and evaluates it. */
    function loadScript(filename: string): unknown;
    /** Reads a whole file as a string, or null when it cannot be read. */
    function loadFile(filename: string): string | null;
    /** Opens a file; mode "r", "w", "a", "r+", ... */
    function open(filename: string, mode: string, error?: Error): StdFile | null;
    function fdopen(fd: number, mode: string, error?: Error): StdFile | null;
    function tmpfile(): StdFile;
    /** strerror(errno). */
    function strerror(errno: number): string;
    /** JSON parse with extensions (Date serialization, ...). */
    function parseExtJSON(str: string): unknown;
    /** Internal: pretty-prints a value for print(). */
    function __printObject(obj: unknown): void;
    /** The standard Error constructor. */
    const Error: ErrorConstructor;
    const SEEK_SET: number;
    const SEEK_CUR: number;
    const SEEK_END: number;
}
