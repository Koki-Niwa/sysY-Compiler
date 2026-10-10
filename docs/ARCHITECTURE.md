# Compiler architecture and contracts

## Actual dependency graph

```text
main -> frontend, structured, ir, opt, support
interpreter -> frontend, support
opt -> analysis, ir, support
analysis -> ir, support
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

S09 mem2reg requires a finalized flat module whose CFG and use-def caches
are consistent and which passes `verifyFlatModule`. It may promote
only entry-block scalar `i32` and `f32` allocas whose uses are direct matching
loads and stores and whose loads are definitely initialized along all
entry-reachable paths. Escaped or derived pointers, aggregates, unsupported
types, unreachable uses, and uninitialized reads remain in memory form. The pass
analyzes before mutation, sorts predecessor/value pairs for generated phis, and
keeps tentative phis unowned until the candidate plan is complete. Failed plans
discard those phis without interning constants; successful candidates remove
only their alloca/load/store operations. The pass preserves block order and
terminators. In F01 it uses IRMutator/CFGEditor for every live mutation;
operand reverse uses stay exact and the predecessor cache needs no rebuilding. Its per-candidate
worklists use finite CFG data and linear temporary storage in blocks, accesses,
and phi inputs; the stage gate supplies the process timeout and memory limits.
Candidate declines do not roll back earlier successful candidates. The API can
be disabled and can capture before/after dumps. The F01 current revision-bound S09 gate passed 54
checks with 0 failures and 2 expected stage skips. The historical
pre-F01 52/0/2 result and corpus limits remain in the
[dated report](../../docs/audit/S09-FRESH-2026-09-30.md). This validates interpreter-observed behavior,
not target runtime performance.

Broad optimization and target backend consumption remain future work.

The Flat IR foundation proceeds through F00–F05 in
`../docs/handoff/SESSION-PLAN.md`. **F00 CLOSED** (accepted revision
`f9425dc56b74b9312f6a336f334e5b347844e830`); **F01 CLOSED / PASS**
(accepted validation revision `22eb7a66d6eb57073ecf904323cf10212fe2c247`,
implementation/Fix1 revision `5f4173caeb8abf78d1ac56c129c9550985f48b0b`);
**F02 implementation present / pending independent validation** (not CLOSED).
F01 accepted implementation and validation revisions remain historical facts;
F02 adds the new analysis foundation without changing F01 revision contracts.

F00 established exact `Value` reverse `Use{Instruction*, operandIndex}`
occurrences across Instructions, Arguments, Constants, GlobalAddr and
BlockAddr, maintained by `addOperand`, `setOperand` and `clearOperands`.
Argument and canonical scalar Constant are independent Value kinds.

### F01 frozen mutation and revision contract

- `IRMutator` is the **normal optimizer data-flow/instruction mutation
  authority**. Its tracked operations include `rewriteOperand`, `replaceUses`,
  `insertInstruction`, `eraseInstruction`, and `replaceAndErase`.
- `CFGEditor` is the **live Phi/CFG mutation authority**. Its operations
  include `rewritePhiValue`, `insertPhi`, `erasePhi`, `redirectEdge`,
  and `splitEdge`. It maintains the predecessor cache incrementally and
  Phi `(pred,value)` pairs in sync with CFG edges, preserving canonical
  predecessor ordering and exact indexed Uses. Duplicate successors have
  explicit semantics: `redirectEdge` acts on a successor **index**;
  redirecting to a target with Phi nodes requires explicit incoming values.
  `splitEdge` handles ordinary and duplicate-target edges; composite
  CFG edits preflight first and commit atomically. Optimizers never rely on
  `rebuildCFG()` after live mutations.
- An Instruction has lifecycle **Detached → Live → Tombstone**. **owned != live**:
  `Module::ownInst()` manages lifetime ownership only; live registry membership
  and BasicBlock membership are independent, verifiable facts.
  `Module::liveInstructions()` enumerates live owned instructions, not every
  owned scratch object. Formal erase makes a Tombstone with `parent=null`,
  not live, no operands, and no successor metadata; reinsertion is forbidden.
  Detached scratch edits do not count as live IR changes.
- F02 pre-validation ownership hardening: every Flat `Value` exposes immutable
  `ownerModule()` provenance. Newly allocated detached Instruction scratch has
  null owner until the first successful `Module::ownInst()`; adoption is
  idempotent in that Module and fails for a foreign-owned Instruction, even
  when detached. `discardDetached()` may delete only genuinely unowned scratch.
  `Argument`, canonical `Constant`, `GlobalAddr`, and `BlockAddr` acquire their
  owner at their respective Module factories, which reject foreign referenced
  globals/blocks and cross-Module/cross-Function parameter registration.
- BasicBlock lifetime ownership is distinct from its structural Function parent:
  raw blocks have `ownerModule() == nullptr`; only `Module::createBlock` assigns
  immutable provenance after confirming the Function belongs to its Module.
  `Function::addBlock` rejects foreign, unowned and duplicate entries; `ownsBlock`
  checks provenance and Module inventory. FlatVerifier V1 independently audits
  every Function block and all Module-owned blocks, including orphan and
  duplicate-membership corruption. No detached block lifecycle is introduced.
- Live operand preflight is centralized in `IRMutator`: Instructions must be
  owned, live, and defined in the user's Function; Arguments must be registered
  in that Function; other Values must belong to the user's Module. RAUW and
  CFG/Phi operations validate all inputs before making live changes. Rejected
  edits preserve reverse Uses, dump and Module/Function revisions. Independent
  FlatVerifier V1 checks operand provenance, including raw-injected foreign
  Values and cross-Function Instruction uses.
- `Module::revision()` and `Function::revision()` track **DataFlow, CFG,
  Memory, Calls, Globals, Interface** categories. F01 actively uses the first
  four; Globals and Interface are reserved fields without dedicated mutation
  APIs. **Applied** edits update actual mutation facts/revisions; **NoChange**
  and **Rejected** do not increment revisions; Detached scratch editing does
  not increment live revisions. Canonical Value RAUW spanning functions grows
  the Module revision and every affected Function revision.
- `MutationSummary` reports `changed`, successful operation count,
  `MutationKind` union and affected Functions. All originate in actually
  applied edits, never caller-declared change flags. They are authoritative
  inputs for F02 analysis management and F03 pass orchestration.
- Existing **Mem2Reg is F01's real tracked consumer**: it uses both
  `IRMutator` and `CFGEditor`; `src/opt` raw mutation = **0**; Mem2Reg
  `rebuildUseDef()` calls = **0** and `rebuildCFG()` calls = **0**.
  Successful outputs pass the verifier directly, without optimizer rebuild.
  `Mem2RegResult.changed` agrees with actual tracked live mutations.
- The architecture-stage boundary gate includes
  `tools/selftest/check_ir_mutation_boundary.py` and
  `tools/selftest/test_ir_mutation_boundary.py`: **44 negative controls PASS**
  and **4 positive controls PASS**. Independent manual negatives show
  `ptr->rawMutation()`, `ref.rawMutation()`, and
  `(*ptr).rawMutation()` cannot bypass the rule. The old tracked/raw
  same-name ambiguity was removed by renaming `IRMutator::setOperand` to
  `rewriteOperand`, and `IRMutator::replaceAllUsesWith` to `replaceUses`.
  `rebuildUseDef()` / `rebuildCFG()` remain reader/builder finalization,
  debug repair and negative-test recovery, never normal optimizer repair.

### F01 frozen acceptance evidence

- **Full O1 Flat textual protocol**: manifest 540; supported 490; **490/490
  byte-identical**, 0 mismatch; tensor baseline rejects **50/50**, current
  rejects **50/50**. Pre-F01 baseline source revision
  `3da451f46bc0e92afe0df9008fbf30b827c7de0f` and compiler SHA256
  `32f8b1dff0444b0cf6e24512edd50cf5695b01e1d4ce9a08a4ac1aba94af63ec`.
  Mutation infrastructure changed; O1 Flat text did not.
- **Current F01 S09**: **54 PASS / 0 FAIL / 2 SKIP** vs historical F00/pre-F01
  **52/0/2**, exactly because `check_ir_mutation_boundary` and
  `test_ir_mutation_boundary` added two architecture gates (52 + 2 = 54).
  Flat-vs-structured: **357 exact**, **62 loop-count-only**, **0 observable
  differences**, **121 categorized skips**. S08: **44 invalid / 7 positive /
  8 legacy**. Mem2Reg structural fixture: **1 alloca / 1 load / 3 stores
  → 0/0/0**, **phi=1**. Forged-stat negative control, `S08_OK` and
  `MEM2REG_OK` all PASS. S09 LLVM: **490** supported modules PASS
  `llvm-as`, **50** tensor frontend rejects, **0 failure / 0 timeout**.
- **Full x86 O1 current-revision execution**:
  **490 PASS / 50 SKIP / 0 FAIL / 0 TIMEOUT**, `mode.optimize=true`,
  `boundary=source`, all **490/490** required compiler invocations include
  `-O1`, revision `22eb7a66d6eb57073ecf904323cf10212fe2c247`,
  `dirty=false`. Initial unsupported runner `--optimize` invocation
  exited 2 (retained harness invocation evidence, **not** product failure);
  rerun with supported `-O1` passed the full matrix.
- **Full no-structured O1 current-revision execution**:
  **490 PASS / 50 SKIP / 0 FAIL / 0 TIMEOUT**, `mode.optimize=true`,
  `boundary=from-flat`. Every required case (490/490) uses
  source → `--emit=flat-ir` → fresh compiler →
  `--from-flat --emit=llvm-ir -O1`: second invocation
  `--from-flat=490/490`, `-O1=490/490`, bad shape **0**. This is the
  serialized Flat → tracked Mem2Reg end-to-end proof.
- **Focused sanity only**: O0 normal **3/3**, O0 no-structured **3/3**,
  AArch64 O1 **3/3**, RV64 O1 **3/3**, and self-built toy O1 **3/3** PASS.
  AArch64/RV64 focused runs are **not** new full target acceptance;
  toy focused is **not** new full 490-case S11b acceptance.
- **Sanitizers**: implementation-stage ASAN **PASS** and UBSAN **PASS**
  with `detect_leaks=0`; final independent F01 validation **did not rerun
  sanitizers**. Known process-lifetime TypePool LSan allocation (~96 bytes)
  remains; **do not claim LSan clean**.

**Boundary**: F01 establishes authoritative, revision-producing IR mutation.
F02 consumes these revisions for cached, dependency-aware, fail-closed
analysis management. F01 did **not** implement `AnalysisManager`, an analysis
cache, dependency invalidation, stale-analysis handles, `PreservedAnalyses`,
`FlatPassManager`, `PassContext`, or `FunctionPass`.

The canonical scalar Constant identity is `(type pointer, raw bits)`: i32 uses
the low 32 bits, i64 the full 64 bits, and f32 the IEEE raw 32-bit pattern.
Thus i32 1 differs from i64 1, positive and negative zero differ, equal NaN
payloads share identity, and different NaN payloads do not. A canonical
Constant has no unique SourceLoc because it may be shared across functions;
Flat dump `@line` presentation is per-function metadata. Object identity and
textual presentation location are separate, preserving frozen dumps during
canonicalization. The former Nop-Instruction parameter model and
`Opcode::ConstantInt` / `Opcode::ConstantFP` payload model have zero active
product references.

The Flat optimization contracts (F01 mutation, F02 cache present; F03+ future) are:

- every `Value` kind owns precise reverse `Use{user, operandIndex}` edges; a
  repeated operand is multiple uses even when it has one distinct user;
- optimization code mutates operands/instruction membership only through an
  `IRMutator`, and CFG/phi structure only through a `CFGEditor`;
- arena/central ownership is allowed for pointer stability, but owned objects
  and live IR membership are distinct; erased/tombstoned instructions are not
  traversed by normal analyses;
- F01 records data-flow/CFG/memory/call revisions (Globals/Interface
  fields reserved); F02 validates caches against declared revision categories
  and dependencies; future F03 `PreservedAnalyses` cannot override
  actual incompatible mutation facts;
- `rebuildUseDef()` / `rebuildCFG()` remain reader/builder finalization and
  verifier/debug reconstruction tools, not the normal maintenance strategy for
  optimization passes;
- `replaceAllUsesWith`, conditional replacement, safe erase and phi incoming
  edits are first-class operations with bidirectional verifier checks;
- Argument is a real `Value`, not an Instruction; scalar constants use canonical
  `(type,bits)` identity while Flat dump preserves historical syntax and per-function
  source-location presentation.

Future analyses are contracts, not empty framework classes. F02 establishes
revision-aware `AnalysisManager`; F03 will establish `FlatPassManager` before S12.
Module/Function scopes land first; CGSCC/Loop scopes are added only when S12/S14
provide real consumers. Analyses state their dependencies and are immutable
results; the first implementation uses invalidate-and-lazy-recompute rather
than complex incremental updaters. Future passes support disabling, verify-each,
before/after dumps, timing/stats and bounded ablation. S09 Mem2Reg is migrated
in F04 as the first formal FunctionPass.

The real `src/analysis/` layer is wired into CMake and the unit runner.
Its permitted imports are `ir/` and `support/` only; `opt/` may depend on
`analysis/`, but `ir/` and `support/` cannot. `DominatorTree` stays a direct
legacy construction in FlatVerifier and its tests until S14.

## F02 AnalysisManager foundation (implementation pending independent validation)

A manager binds one `flat::Module` for its entire lifetime; it owns Module
and Function cache entries, with type-index analysis identity. Function requests
reject foreign parent modules. Results are immutable shared ownership held by
typed handles; handles observe manager lifetime through a weak token and only
return a `const Result*` when their generation remains current. Manager
destruction and stale generations cause `get() == nullptr` without dangling
pointers.

Each analysis declares a dependency mask over F01 DataFlow, CFG, Memory,
Calls, Globals, Interface revisions (None means no IR dependencies). Function
entries observe Function revisions, Module entries observe Module revisions;
unrelated categories and other Function edits leave caches intact. While
computing, the manager automatically records each requested analysis identity
and generation. Freshness recursively validates these dependencies, so
transitive invalidation works even when a parent declares None. Cycles,
failed dependencies, and failed computations never cache partial results.
Revision mismatches invalidate logically; the next request lazily recomputes.

`ReachabilityAnalysis` is a pure read-only Function analysis depending on CFG
only, using the original entry-based terminator-successor stack algorithm.
Mem2Reg consumes it for each candidate before mutation. No mutation callback,
PassManager, PreservedAnalyses or DominatorTree migration is part of F02.

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
