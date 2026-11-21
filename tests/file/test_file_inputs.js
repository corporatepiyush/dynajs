// flags: --std
// covers: contract dyna:file.* -- signature-driven input sweep of every declared name
// Signature-driven input sweep of dyna:file (tests/lib/sigfuzz.js): every name
// dynajs.d.ts declares gets its baseline, the edge values of each parameter in
// turn, wrong types, and hostile coercions that close the receiver mid-call.
// Fails on a call over its time budget, a non-Error throw, a changed built-in
// prototype, a name no call ever got past the argument boundary, or a class
// no instance could be built for (its methods would silently go untested).
import { inputSweep } from "../lib/sigfuzz.js";

await inputSweep("test_file_inputs", "file");
