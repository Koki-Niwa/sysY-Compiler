# Compiler architecture and contracts

## Actual dependency graph

```text
main -> frontend, structured, ir, support
structured core -> ir, support
structured bridge -> frontend (only IRGenInternal.h and IRGenImpl.h)
frontend -> support
ir -> support
support -> nothing in the compiler
```

The graph describes source dependencies, not pipeline order. `IRGen` is the
intentional AST-to-IR bridge. Structured reader/verifier core must not acquire
frontend types. Runtime declarations live in the neutral catalog
`support/RuntimeLib.h`; the frontend adapts it to frontend types, while the
structured reader/verifier query the neutral result kind directly. The catalog
describes source-visible signatures, not the entire lowered backend ABI.

## Ownership and reading order

| Area | Owns | Read first |
|---|---|---|
| `frontend/` | source syntax, types, diagnostics, runtime type adaptation | `Lexer`, `Parser`, `Sema` |
| `structured/` | regions, loop/access structure, AST-to-IR bridge, structured verification | `StructuredIR`, `StructuredVerifier`, `IRGen`, `IRGenInternal.h` |
| `ir/` | flat values, blocks, instructions, CFG/SSA data, verification, and LLVM text emission | `Module`, `Function`, `BasicBlock`, `Instruction`, verifier, `IREmitter` |
| `support/` | diagnostics, source files/locations, neutral catalogs | `Diagnostic`, `SourceFile`, `RuntimeLib` |
| `main/` | command-line mode selection and artifact policy | `main.cpp`, emitter entry point |

## Invariants

- Frontend variable reads remain explicit loads until the owned promotion pass.
- Structured regions are verified before flattening; flattening is a one-way
  reviewed boundary for a given pipeline.
- Flat blocks have explicit terminators and valid predecessor/successor links;
  SSA uses and definitions are checked by the verifier.
- Integer arithmetic preserves 32-bit wraparound; floating-point transforms
  preserve required evaluation order and do not assume fast-math semantics.
- Target-independent `.ll` omits triple and data layout. Backend configuration
  supplies the actual target. The frontend must not infer a host target.
- Unsupported modes fail with a stable diagnostic and no partial artifact.
- The S07 emitter preserves the LLVM `double` type of `FPExt` through the
  `putf` variadic argument and emits `declare void @putf(ptr, ...)`; flat IR's
  placeholder `f32` type cannot be used as the emitted ABI type. `FPExt` is
  rejected outside that boundary. Quoted global identifiers escape quote,
  backslash, and control bytes as LLVM `\\xx` byte escapes.
- Tensor corpus entries count as expected rejects only when the frontend emits
  either the `E-UNDEF` diagnostic for the identifier `tensor` or a parser
  diagnostic explicitly rejecting `tensor` as a declaration token, exits with
  status 1, and produces no `.ll`; each accepted rejection retains stderr in
  its result. An explicit `--stage S07` run does not execute S11 target-runtime
  checks.

## Implemented versus future

Implemented today: frontend and structured/flat IR construction, structured
verification, loop normalization, flattening, dumps, and the S07 target-neutral
LLVM text emitter. `IREmitter` owns the consumer interface; `LLVMTextEmitter`
owns the LLVM dialect mapping and rejects an unsupported module atomically.
Mem2reg, broad optimization, and target backend consumption remain future work.

Future analyses are contracts, not empty framework classes. Each must state its
input, output, preserved/invalidated analyses, resource budget, diagnostics,
failure atomicity, and before/after dump format before being wired into a
pipeline. Future passes must support disabling and before/after dumps. A general
pass manager is not implemented yet.

## Validation commands

Run from the compiler repository (the full gate also needs sibling test/runtime assets):

```bash
cmake -S . -B build -G Ninja
cmake --build build
python3 tools/selftest/check_architecture.py --root .
python3 tools/selftest/check_line_budget.py --root .
bash tools/verify.sh --stage S07
```

Use the exact gate available in the checkout; do not describe a planned gate
as a completed result. Historical reports under workspace `../docs/audit/` retain their
original commands and verdicts.
