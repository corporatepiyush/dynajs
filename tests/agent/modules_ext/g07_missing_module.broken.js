// modules_ext g07 (expected-parse-failure): static import of a nonexistent dyna:
// module must fail at parse/link time with SyntaxError/ReferenceError (name parity
// across engines is checked on the dynajs side only; node lacks dyna: entirely).
import * as nope from "dyna:definitely_not_a_module";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
out('SHOULD NOT RUN');
