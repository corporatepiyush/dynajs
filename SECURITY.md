# Security Policy

## Reporting a vulnerability

Report privately through **GitHub Security Advisories** — the "Report a
vulnerability" button under this repository's Security tab. That opens a private
thread with the maintainer; do not open a public issue for a suspected
vulnerability.

Please include the shortest input that reproduces the issue, the build
configuration (`CONFIG_NATIVE_MODULES`, `CONFIG_TLS`, `CONFIG_IO_URING`,
`CONFIG_OPENLIBM`), the platform, and whether it reproduces under
`./dev.sh gate` (ASan + UBSan).

A fuzzer-produced crash is most useful with the reproducer file itself. An
unshrunk case is still worth sending — "a 4096-byte input inside a 3-deep
object" is hard to act on, but it is better than silence.

## What is in scope

- **Memory safety** anywhere in the engine or the `dyna:*` native modules:
  out-of-bounds read or write, use-after-free, double free, type confusion.
- **The deserializer** (`bc_read`) is the top untrusted surface. It reads a
  binary format and indexes tables by values taken from that format.
- **The network stack**: HTTP request framing and smuggling, header parsing,
  the L7 proxy's header re-serialisation, TLS certificate and hostname
  verification, and the DB drivers' wire parsers.
- **Resource exhaustion reachable from a single peer or a single input** —
  unbounded memory, unbounded recursion, or work that grows superlinearly in
  attacker-chosen input. Bounded-but-slow is a bug; unbounded is a
  vulnerability.
- **Sandbox escapes** where a documented restriction is bypassed: reaching the
  filesystem or network from a build that did not enable those modules, or
  reaching `Object.prototype` through a parser.

## What is NOT in scope

- **Running arbitrary untrusted JavaScript.** This engine does not claim to be
  a security boundary against hostile script. Removing privileged functions and
  I/O does not make a JavaScript VM a sandbox; RCE bugs are found regularly in
  engines with far more hardening effort behind them. If you need to execute
  untrusted code, isolate it at the OS or hypervisor level and treat the engine
  as untrusted.
- Denial of service that requires the operator's own configuration to be
  hostile, or that needs privileged local access.
- Findings against a build with sanitizers disabled that cannot be reproduced
  with them enabled — send them anyway, but they take longer to confirm.

## Supported versions

This project has not yet cut a tagged release. Until it does, only `master` is
supported, and fixes land there. Consumers pinning a commit should follow
`master` for security fixes.

## Handling

Reports are acknowledged as they are read, not on a contractual clock — this is
a small project and promising a 24-hour SLA would be a promise it cannot keep.
Expect a first substantive reply within a week. Fixes for confirmed memory-safety
and network-facing issues are prioritised above everything else in the queue.

Credit is given in the advisory unless you ask otherwise.
