/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:metrics
/** dyna:metrics — a process-wide registry of counters, gauges and histograms with a Prometheus text scrape; dyna:http's App records into it and serves it at /metrics.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:metrics" {
    /** A registry of counters, gauges and histograms with a Prometheus text scrape; at most 256 series (RangeError beyond). */
    const Metrics: {
        /** Increments a counter; a negative or NaN increment is refused. */
        counter(name: string, value?: number, labels?: Record<string, string>): void;
        /** Sets a gauge to a value. */
        gauge(name: string, value: number, labels?: Record<string, string>): void;
        /** Records an observation; buckets are 5ms..1s by default, or 1..6 increasing `opts.buckets` edges fixed at the series' first use. */
        histogram(name: string, value: number, labels?: Record<string, string>, opts?: { buckets: number[] }): void;
        /** The registry as Prometheus text exposition. */
        scrape(): string;
        /** Empties the registry; intended for tests. */
        reset(): void;
    };

}

