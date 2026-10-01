const OP = { HALT: 1 };
try { eval("const OP = { HALT: 2 };"); console.log("eval-ok"); } catch (e) { console.log("eval threw", e.constructor.name); }
console.log(OP.HALT);
