# Compiler repository guide

This file is the standalone entry point for work inside `compiler/`. The
workspace handoff in `../docs/handoff/` is secondary context; read only the
current state and the contract for the module being changed.

## Principles and product objective

Correctness and the established delivery constraints are hard gates. For
architecture choices, apply Codex readability first and functional modularity
second: prefer small interfaces, explicit ownership, and modules one session
can understand. A shorter or newer abstraction is not a reason to weaken an
invariant or hide state.

After correctness gates pass, make generated-assembly runtime performance the
product objective. Treat compile time, peak RSS, output size, and code size as
explicit costs.

Within those constraints, prefer the architecture one fresh session can
understand. Multi-level IR retains information while a transformation can use
it, then lowers through a verified boundary. Infrastructure may enable later
optimizations without claiming an immediate runtime gain.

## Before editing

- Read [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md).
- Read the relevant module contract in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
- For optimization work, read [`docs/PERFORMANCE-EXTENSIONS.md`](docs/PERFORMANCE-EXTENSIONS.md).
- For backend, target-cost, RA, scheduling, or middle/backend feedback work,
  also read [`docs/BACKEND-PERFORMANCE-PLAN.md`](docs/BACKEND-PERFORMANCE-PLAN.md);
  RA/scheduler experiments use [`docs/RA-LAB.md`](docs/RA-LAB.md).
- Check includes with `python3 tools/selftest/check_architecture.py --root .`.
  Run the complete current gate with `bash tools/verify.sh --stage S06`.
  `tools/gate/*.sh` are sourced modules, not standalone entry points.

## Module rules

The supported dependency direction is:

```text
main -> frontend, structured, ir, opt, support
structured core -> ir, support
opt -> ir, support
frontend -> support
ir -> support
support -> nothing in the compiler
```

The two deliberate bridge headers are
`structured/IRGenInternal.h` and `structured/IRGenImpl.h`; they connect the
frontend-facing generator to the structured core. Keep runtime signature
lookup in `support/RuntimeLib.h` and let the frontend adapt it to its AST/type
model. Do not create another runtime table in a lower layer.

Product `.h/.cpp/.inc` files have a hard limit of 600 lines. `verify.sh` and
`gate/*.sh` have the same hard limit; existing tools use a non-growing ratchet
and new tools have an 800-line hard limit. Passing the limit does not replace
checking state ownership and function complexity.

## Pass and diagnostic contract

Every pass documents its input, output, preserved and invalidated analyses,
resource budget, diagnostics, and failure atomicity. Future passes must be
individually disabled for comparison and support optional before/after dumps.
A pass may decline to transform when its preconditions are unproven. Normalizers may be
idempotent; iterative analyses and bounded search must state their convergence
or budget rule instead of claiming universal idempotence.

Do not add function-name, test-name, input-size, or host-target special cases.
Compiler code does not invoke external tools. Use independent reviewers,
negative controls, and visible gate commands; authors may run those gates, and
a report may claim only the tests actually run. Do not describe a visible gate
as hidden or inaccessible. Full corpus gates may require the workspace's
external test corpus and runtime assets; a standalone clone must still build
and pass its portable local checks without those assets.

Facts from source and audits are scoped by topic. They inform a decision only
when they agree with the user's request and this file's rules; they never
override either one.

Milestone scheduling is unified in [`../docs/handoff/SESSION-PLAN.md`](../docs/handoff/SESSION-PLAN.md):
B00–B20 are part of the implementation flow after S11b, not work deferred
until after S28. Keep historical S IDs stable and follow the S/B wave dependencies.
