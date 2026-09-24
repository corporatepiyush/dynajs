#!/usr/bin/env python3
"""Generate cpool-boundary test files: N prior double constants, then folds.
The int-fold result and the folded wide string land at cpool index ~N —
the historical miscompile class was cpool idx >= 256 re-emitting the unfolded
(or malformed) operand."""
import os
# Writes the a_cpool_*.js probes next to their committed siblings by default
# (the committed files ARE this generator's output), or into argv[1].
import sys
DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
SIZES = [250, 254, 255, 256, 257, 260]
for n in SIZES:
    lines = [f"// cpool boundary: {n} prior double constants, then folds near idx {n}..{n+4}"]
    for i in range(n):
        lines.append(f"const c{i} = {i}.25;")  # every literal unique -> n distinct cpool entries
    # keep every const alive (one big print of a checksum)
    lines.append("let cs = 0;")
    for i in range(n):
        lines.append(f"cs += c{i};")
    lines.append('console.log("cs", cs);')
    # int32 const fold AFTER the boundary
    lines.append('console.log("ifold", 1 + 2 * 3, (1 + 2 * 3) + c0, 2147483647 + 0, 40 + 2);')
    # wide string fold (surrogates) AFTER the boundary
    lines.append('const ws = "\\u{1F600}" + "x!";')
    lines.append('console.log("ws", ws, ws.length, ws.charCodeAt(0).toString(16), ws.charCodeAt(1).toString(16), ws.charCodeAt(2).toString(16));')
    # numeric-string fold after the boundary
    lines.append('console.log("ns", "12" + "34", ("12" + "34") === "1234");')
    # another wide fold two slots later
    lines.append('const ws2 = "a" + "\\u{1F601}";')
    lines.append('console.log("ws2", ws2.length, ws2.charCodeAt(0).toString(16), ws2.charCodeAt(1).toString(16));')
    # eval after boundary (fresh compile unit must be unaffected)
    lines.append('console.log("ev", eval("1+2*3"), eval("\'q\' + \'r\'"));')
    path = os.path.join(DIR, f"a_cpool_{n}.js")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path)
