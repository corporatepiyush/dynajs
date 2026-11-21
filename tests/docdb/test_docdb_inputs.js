// flags: --std
// covers: contract dyna:docdb.* -- signature-driven sweep of every declared name
// Signature-driven input sweep of dyna:docdb (tests/lib/sigfuzz.js): every name dynajs.d.ts declares gets its
// baseline, the edge values of each parameter in turn, wrong types, and hostile coercions. Fails on a call over its
// time budget, a non-Error throw, a changed built-in prototype, a name no call ever got past the argument boundary,
// or a class no instance could be built for.
import { inputSweep } from "../lib/sigfuzz.js";

await inputSweep("test_docdb_inputs", "docdb");
