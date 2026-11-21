/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:cli
/** dyna:cli — terminal programs: argument parsing (Command), ANSI styling, prompts, raw-mode key input, progress bars. */
declare module "dyna:cli" {
    // $DYNAJS_BYTECODE_CACHE is a developer speedup over TRUSTED input: never enable it for scripts in directories writable by others.

    /** Styles text with a named ANSI style or list (also "256:n", "#rrggbb", "rgb:r,g,b" and bg forms); escapes drop under $NO_COLOR or a non-TTY; unknown names throw. */
    function StyleText(style: string | string[], text: string): string;
    /** The list of styles the engine can apply. */
    function Styles(): string[];
    /** True when the stream with the given fd (default stdout) is a TTY. */
    function IsTTY(fd?: number): boolean;
    /** Terminal columns: TIOCGWINSZ on fd (stdout default), else $COLUMNS, else 80. */
    function Columns(fd?: number): number;
    /** The terminal color depth: 0 (none), 4 (16 colors), 8 (256), or 24 (truecolor). */
    function ColorDepth(fd?: number): number;

    /** Prints `message` and reads one stdin line; empty or EOF yields `opts.default` (else ""); an answer over 1 MiB is a RangeError. */
    function prompt(message: string, opts?: { default?: string }): string;
    /** Prints `message` and reads a line: "y"/"yes" (case-insensitive) is true, anything else including EOF is false. */
    function confirm(message: string): boolean;
    /** Arrow-key menu in raw mode; returns the chosen option, or null on Escape/Ctrl-C/EOF. The terminal mode is restored on every exit path. */
    function select(message: string, options: string[]): string | null;
    /** Reads one key event in raw mode; null at EOF. A lone ESC is told from a sequence head by a 50 ms quiet window. */
    function keypress(): {
        /** "up", "enter", "a", ... -- null for an unrecognized sequence. */
        name: string | null;
        /** The bytes the event consumed, UTF-8 decoded (malformed runs appear as U+FFFD). */
        sequence: string;
        ctrl: boolean;
        meta: boolean;
        shift: boolean;
    } | null;

    /** A single-line progress bar with a time-based ETA. */
    class ProgressBar {
        constructor(opts: { total: number });
        /** Redraws the bar at `n` of `total`; reaching `total` finishes the line and later updates are refused. */
        update(n: number): this;
    }

    /** A single-line spinner over the frames "|/-\". */
    class Spinner {
        constructor(opts?: { text?: string });
        /** Starts the animation; chainable. */
        start(): this;
        /** Advances one frame and redraws. */
        tick(): this;
        /** Clears the line, or leaves `text` as the final line (with a newline). */
        stop(text?: string): this;
    }

    /** Renders rows as aligned columns ("grid"), TSV, or RFC 4180 CSV. */
    function Table(rows: (string | number)[][], opts?: {
        /** Header row, rendered first ("grid" rules it off with dashes). */
        head?: string[];
        /** Per-column alignment; "left" is the default for unlisted columns. */
        align?: ("left" | "right" | "center")[];
        /** "grid" (default), "tsv", or "csv". */
        format?: "grid" | "tsv" | "csv";
    }): string;

    /** A command-line parser: options, positional arguments, subcommands, generated help. */
    class Command {
        /** Constructs an argument parser built from declarative specs. */
        constructor(name?: string);
        get name(): string;
        /** Describes this command for help output. */
        describe(text: string): this;
        /** Registers an option; precedence is CLI > `env` variable > default. Numbers use a strict finite-decimal grammar ("NaN", "0x10" are refused). */
        option(flags: string, description?: string, opts?: { type?: "boolean" | "string" | "number"; required?: boolean; variadic?: boolean; default?: unknown; env?: string }): this;
        /** Registers a positional argument. */
        argument(name: string, description?: string): this;
        /** Registers a subcommand. */
        command(sub: Command): this;
        /** Permits unknown options instead of refusing them. */
        allowUnknown(v: boolean): this;
        /** Registers the handler a leaf parse calls with (options, args); its return lands on the parse result's `result`. */
        action(fn: (options: Record<string, unknown>, args: string[]) => unknown): this;
        /** Sets the version and enables `--version` (chainable); with no argument returns the version ("" when unset). */
        version(v: string): this;
        version(): string;
        /** Parses argv (default scriptArgs) into { options, arguments, command }; a subcommand's own parse nests under `result`, so a leaf action's return is `.result.result`. Pass T to narrow. A NUL byte in argv is a TypeError. */
        parse<T = { options: Record<string, unknown>; arguments: string[]; command: string | null }>(args?: string[]): T;
        /** Prints the help text. */
        help(): string;
    }
}
