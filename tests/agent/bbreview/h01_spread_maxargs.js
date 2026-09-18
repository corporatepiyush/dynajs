// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~n1024", "~n1024"], "~n65535", "~n65536", "~n65537", "~n100000"];
__EXP[1] = [["n65535 ERR RangeError", "n200000 ERR RangeError"], "n65536 ERR RangeError", "n65537 ERR RangeError", "n100000 ERR RangeError", "n200000 ERR RangeError"];
__REQ = {"dynajs": {"0": 1, "1": 5}, "node": {"0": 5, "1": 1}};
// H: spread-into-call arg-count limit probe
function id() { return arguments.length; }
for (const n of [1024, 65535, 65536, 65537, 100000, 200000]) {
  try {
    const a = new Array(n).fill(0);
    __L(0, "n" + n, id(...a));
  } catch (e) { __L(1, "n" + n, "ERR", e.constructor.name); }
}

summary("bbreview");
