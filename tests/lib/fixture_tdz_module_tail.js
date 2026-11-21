// Fixture for tests/core/test_tdz_elision.js (module tail): the module
// function compiles to bytecode whose last opcode is one byte long and ends
// exactly at its buffer, with a TDZ check (on t) for the elision pass to
// scan. The pass once read a u16 operand for every opcode, past that end.
// Byte-exact: any edit to the code lines below can hide the case.
const t = 1;
function f() {}
if (t) Object();
