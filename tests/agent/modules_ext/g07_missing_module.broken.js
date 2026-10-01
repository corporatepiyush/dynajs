import * as nope from "dyna:definitely_not_a_module";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
out('SHOULD NOT RUN');
