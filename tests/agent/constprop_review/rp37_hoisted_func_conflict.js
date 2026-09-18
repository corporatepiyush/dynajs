try { eval("const OP = { A: 1 }; function OP() { return 2; }"); console.log("no-err"); }
catch (e) { console.log("threw", e.constructor.name); }
try { eval("function OP() { return 2; } const OP = { A: 1 };"); console.log("no-err2"); }
catch (e) { console.log("threw2", e.constructor.name); }
