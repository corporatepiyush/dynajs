// modules_ext g06 (expected-parse-failure): duplicate import binding of the same
// name from two modules must be a SyntaxError in BOTH engines. Runner treats
// nonzero exit + engine error-name match as PASS. (Broken-by-design probe.)
import { cwd } from "dyna:sys";
import { cwd } from "dyna:sys";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
out('SHOULD NOT RUN');
