import * as a from "../core/test_cyclic_import.js"
export function f(x) { return 2 * a.g(x) }
