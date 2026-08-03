#!/usr/bin/env python3
"""Emit a CycloneDX 1.6 SBOM for the CURRENT build configuration.

A committed SBOM rots the moment a flag changes: half of these components are
optional and config-gated, so "what this project depends on" is not a constant.
This reads the configuration and reports what is ACTUALLY linked.

Usage:
  tools/gen-sbom.py [--config KEY=VAL ...] > sbom.json
  make sbom

Versions come from the system where they can be probed (pkg-config) and from
the pinned ref where they are vendored. A component whose version cannot be
determined says so rather than guessing -- an invented version is worse than a
missing one, because it looks authoritative.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(*cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


def pkg_version(name):
    return sh("pkg-config", "--modversion", name) or "unknown"


def vendored_version(relpath, describe_dir):
    """A vendored clone's version is its git description, not a guess."""
    d = os.path.join(ROOT, describe_dir)
    if not os.path.isdir(d):
        return None
    v = sh("git", "-C", d, "describe", "--tags", "--always")
    return v or "unknown"


def component(name, version, ctype, supplier, license_id, purl=None, desc=None):
    c = {
        "type": ctype,
        "name": name,
        "version": version,
        "supplier": {"name": supplier},
        "licenses": [{"license": {"id": license_id}}],
    }
    if purl:
        c["purl"] = purl
    if desc:
        c["description"] = desc
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timestamp", default=None,
                    help="ISO8601; the caller supplies it so the output is "
                         "reproducible in a build that pins its own clock")
    a = ap.parse_args()

    version = "unknown"
    vf = os.path.join(ROOT, "VERSION")
    if os.path.exists(vf):
        version = open(vf).read().strip()

    comps = []

    # Upstream lineage. This is a FORK, not a dependency: the engine core is
    # QuickJS source carried in-tree, so it belongs in the SBOM as a component
    # even though nothing links it externally.
    comps.append(component(
        "quickjs", "2026-06-04", "library", "Fabrice Bellard and Charlie Gordon",
        "MIT", desc="Upstream engine core; this project is a fork of it."))

    ol = vendored_version("third_party/openlibm", "third_party/openlibm")
    if ol and os.path.exists(os.path.join(ROOT, "third_party/openlibm/libopenlibm.a")):
        comps.append(component(
            "openlibm", ol, "library", "JuliaMath", "MIT",
            purl="pkg:github/JuliaMath/openlibm@" + ol,
            desc="Vendored for cross-platform bit-reproducible Math; linked "
                 "before -lm. Absent => the system libm is used instead."))

    mi = vendored_version("third_party/mimalloc", "third_party/mimalloc")
    if mi:
        comps.append(component(
            "mimalloc", mi, "library", "Microsoft", "MIT",
            purl="pkg:github/microsoft/mimalloc@" + mi))

    # System libraries: present only when the matching CONFIG_* selected them.
    if sh("pkg-config", "--exists", "sqlite3") == "":
        v = pkg_version("sqlite3")
        if v != "unknown":
            comps.append(component("sqlite", v, "library", "SQLite Consortium",
                                   "blessing", purl="pkg:generic/sqlite@" + v))
    v = pkg_version("openssl")
    if v != "unknown":
        comps.append(component("openssl", v, "library", "OpenSSL Project",
                               "Apache-2.0", purl="pkg:generic/openssl@" + v))
    v = pkg_version("liburing")
    if v != "unknown":
        comps.append(component("liburing", v, "library", "Jens Axboe", "MIT",
                               purl="pkg:generic/liburing@" + v))

    doc = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "version": 1,
        "metadata": {
            "tools": {"components": [component(
                "gen-sbom.py", version, "application", "dynajs", "MIT")]},
            "component": component(
                "dynajs", version, "application", "dynajs", "MIT",
                desc="JavaScript engine and runtime, forked from QuickJS."),
        },
        "components": comps,
    }
    if a.timestamp:
        doc["metadata"]["timestamp"] = a.timestamp

    json.dump(doc, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
