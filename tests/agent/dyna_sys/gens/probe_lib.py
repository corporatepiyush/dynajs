"""probe_lib.py — shared emit helper for dyna_sys probe generators.

dynajs parses a file as a SCRIPT unless it starts with import declarations,
so emitted probes are: [CTL comment] + imports + harness + body.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = open(os.path.join(HERE, "..", "h.js")).read()
PROBES = os.path.join(HERE, "..", "probes")


def emit(module, name, imports, code, ctl=None, summary_tag=None):
    """Write probes/<module>/<name>.js. `imports` = import block (goes first),
    code = probe body (harness prepended, so assert*/summary are in scope)."""
    d = os.path.join(PROBES, module)
    os.makedirs(d, exist_ok=True)
    tag = summary_tag or (module + "." + name)
    head = ("// CTL:%s\n" % ctl) if ctl else ""
    src = (head + imports.rstrip() + "\n" + HARNESS +
           "\n// ---- generated probe %s/%s ----\n" % (module, name) + code)
    p = os.path.join(d, name + ".js")
    with open(p, "w") as f:
        f.write(src)
    return p
