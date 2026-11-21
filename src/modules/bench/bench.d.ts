/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:bench
/** dyna:bench — a micro-benchmark harness: bench() times a function in adaptive batches on the monotonic clock, table() prints the results. */
declare module "dyna:bench" {
    /** One result: opsPerSec = 1000/meanMs, rsd = std/mean. Samples are batches doubled until each lasts 0.1 ms, so tiny bodies are not measured against the clock tick. */
    interface BenchResult {
        name: string;
        iters: number;
        elapsedMs: number;
        opsPerSec: number;
        rsd: number;
        p50Ms: number;
        p99Ms: number;
        meanMs: number;
    }
    /** Runs fn for timeMs (default 500) after warmupMs (default 100) and records the result for table(). */
    function bench(name: string, fn: () => void, opts?: { timeMs?: number; warmupMs?: number; warmup?: number }): BenchResult;
    /** Tab-separated table of all bench() results this process (header + a row per run), retained for the process lifetime; no reset API. */
    function table(): string;
}