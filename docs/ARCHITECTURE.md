# Compiler architecture and contracts

## Actual dependency graph

```text
main -> frontend, structured, ir, opt, support
interpreter -> frontend, support
opt -> ir, support
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
also assigns each source-visible runtime signature one reference-execution
behavior, so the interpreter does not duplicate runtime-name dispatch. These
labels do not prove purity or describe the entire lowered backend ABI.
The S10a reference interpreter is built by its isolated self-test, not linked
into the compiler CLI. It consumes analyzed AST and depends on frontend/support
contracts only; it must not call IR generation, either IR, LLVM, or target tools.
Seeded generation serializes bounded recipes and source text; the separate
`s10a_oracle` evaluates recipe fields without parsing or executing that text and
shares no expression-building helper with the generator or interpreter. The
runner's optional `--report` path writes an atomic RUNNING/FAIL/PASS manifest.
A PASS manifest requires a clean compiler revision, exact fixed/generated
counts, both negative controls, the oracle version, empty stderr, and hashes of
the retained artifacts and relevant interpreter self-test sources.

## Ownership and reading order

| Area | Owns | Read first |
|---|---|---|
| `frontend/` | source syntax, types, diagnostics, runtime type adaptation | `Lexer`, `Parser`, `Sema` |
| `interpreter/` | direct execution of Sema-annotated AST as a reference oracle; owns values, storage, control flow, and runtime I/O | `Interpreter.h`, `Interpreter.cpp` |
| `structured/` | regions, loop/access structure, AST-to-IR bridge, structured verification | `StructuredIR`, `StructuredVerifier`, `IRGen`, `IRGenInternal.h` |
| `ir/` | flat values, blocks, instructions, CFG/SSA data, verification, and LLVM text emission | `Module`, `Function`, `BasicBlock`, `Instruction`, verifier, `IREmitter` |
| `opt/` | flat-IR transformations over verified modules | `Mem2Reg` |
| `support/` | diagnostics, source files/locations, neutral catalogs | `Diagnostic`, `SourceFile`, `RuntimeLib` |
| `main/` | command-line mode selection and artifact policy | `main.cpp`, emitter entry point |

## Invariants

- Frontend variable reads remain explicit loads until the owned promotion pass.
- Structured regions are verified before flattening; flattening is a one-way
  reviewed boundary for a given pipeline.
- Flat blocks have explicit terminators and valid predecessor/successor links;
  SSA uses and definitions are checked by the verifier.
- `verifyFlatModule` first validates block and instruction ownership, dense
  indices, terminator shape, successor membership, and predecessor cache
  equality. It recomputes predecessor membership from validated terminators,
  collapsing duplicate edges from one source, and requires the cached vector
  to remain in strict block-index order. Non-terminators other than phi nodes
  carry no block successors; malformed structure does not enter indexed CFG or
  dominance analysis.
- Flat opcode arity and operand/result type legality live in the dependency
  leaf `ir/FlatSemantics`. The verifier and LLVM emitter share this contract;
  LLVM-only ABI and representability checks remain in the emitter boundary.
- Phi nodes have one incoming value per distinct predecessor, cover that set
  exactly, and use a value type equal to the phi result type. Dominance is
  false for foreign or unreachable blocks, including unreachable self pairs.
- Global-address operands must name an object in the same module and have that
  object's exact address type; the verifier and emitter both reject null,
  foreign, or retyped addresses.
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

S09 mem2reg requires a flat module whose CFG and use-def caches were rebuilt
after prior structural edits and which passes `verifyFlatModule`. It may promote
only entry-block scalar `i32` and `f32` allocas whose uses are direct matching
loads and stores and whose loads are definitely initialized along all
entry-reachable paths. Escaped or derived pointers, aggregates, unsupported
types, unreachable uses, and uninitialized reads remain in memory form. The pass
analyzes before mutation, sorts predecessor/value pairs for generated phis, and
keeps tentative phis unowned until the candidate plan is complete. Failed plans
discard those phis without interning constants; successful candidates remove
only their alloca/load/store operations. The pass preserves block order and
terminators, then rebuilds CFG and use-def caches after commits. Its per-candidate
worklists use finite CFG data and linear temporary storage in blocks, accesses,
and phi inputs; the stage gate supplies the process timeout and memory limits.
Candidate declines do not roll back earlier successful candidates. The API can
be disabled and can capture before/after dumps. The current revision-bound S09 gate passed 52
checks with 0 failures and 2 expected stage skips; the [dated report](../../docs/audit/S09-FRESH-2026-09-30.md)
records corpus counts and limits. This validates interpreter-observed behavior,
not target runtime performance.

Broad optimization and target backend consumption remain future work.

Before S12/S17 work expands, the Flat IR foundation must be upgraded in the
F00–F05 slices recorded in `../docs/handoff/SESSION-PLAN.md`. The current
`operands + Instruction::users + rebuildUseDef/rebuildCFG` model remains a
construction/debug mechanism, not the long-term mutation contract.

The planned Flat optimization invariants are:

- every `Value` kind has precise reverse `Use{user, operandIndex}` edges; a
  repeated operand is multiple uses even when it has one distinct user;
- optimization code mutates operands/instruction membership only through an
  `IRMutator`, and CFG/phi structure only through a `CFGEditor`;
- arena/central ownership is allowed for pointer stability, but owned objects
  and live IR membership are distinct; erased/tombstoned instructions are not
  traversed by normal analyses;
- mutation records data-flow/CFG/memory/call/interface revisions. Analysis
  cache validity is checked against those revisions; a pass's
  `PreservedAnalyses` declaration cannot override a real incompatible mutation;
- `rebuildUseDef()` / `rebuildCFG()` remain reader/builder finalization and
  verifier/debug reconstruction tools, not the normal maintenance strategy for
  optimization passes;
- `replaceAllUsesWith`, conditional replacement, safe erase and phi incoming
  edits are first-class operations with bidirectional verifier checks;
- Argument becomes a real `Value`, not a Nop instruction; scalar constants
  converge on canonical `(type,bits)` values even if stable dump syntax keeps
  its historical textual form.

Future analyses are contracts, not empty framework classes. F02/F03 establish
the minimal revision-aware `AnalysisManager` and `FlatPassManager` before S12.
Module/Function scopes land first; CGSCC/Loop scopes are added only when S12/S14
provide real consumers. Analyses state their dependencies and are immutable
results; the first implementation uses invalidate-and-lazy-recompute rather
than complex incremental updaters. Future passes support disabling, verify-each,
before/after dumps, timing/stats and bounded ablation. S09 Mem2Reg is migrated
in F04 as the first formal FunctionPass.

The future `analysis/` source layer is not considered implemented merely because
an executable checker knows its name. When that directory first lands, the
module graph in this document, `AGENTS.md`, `check_architecture.py` and negative
architecture tests must change atomically; pre-authorized undocumented
dependencies are not a supported contract.

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
