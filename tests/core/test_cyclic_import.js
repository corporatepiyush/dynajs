// covers: c:src/modules/libc/libc-module-loader.c
import {assert} from "../lib/assert.js"
import {f} from "../lib/fixture_cyclic_import.js"
import { sections } from "../lib/kit.js";
export {f}
export function g(x) { return x + 1 }
const S = sections("test_cyclic_import");
S.section("a function imported through a module cycle sees the importer's export", () => {
    assert(f(1), 4)
});
S.end();
