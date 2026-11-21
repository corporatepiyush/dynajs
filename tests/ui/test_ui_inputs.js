// flags: --std
// covers: contract dyna:ui.* -- signature-driven input sweep of every declared name
// Signature-driven input sweep of dyna:ui (tests/lib/sigfuzz.js): every name dynajs.d.ts declares
// gets its baseline, each parameter's edge values, wrong types and hostile coercions. Widgets exist
// only inside frame(), so sigfuzz_baselines.js's CALL hook runs each call inside one (except the
// names refused there). A build without CONFIG_UI: SKIP.
import { inputSweep } from "../lib/sigfuzz.js";

await inputSweep("test_ui_inputs", "ui");
