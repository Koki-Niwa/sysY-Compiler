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
**F02 CLOSED / PASS** (accepted revision
`7fdb9f33f254bc0219269cafdc8e0b5e36d2e6a9`; valid 004 validation
and independent 008 evidence review); **F03 CLOSED / PASS** (014
acceptance, implementation `b1c06bb936049fc713c0db16eef8015b7b3ab2ec`);
**F04 CLOSED / PASS** (commit `10a3cbd6`); **F05 CLOSED / PASS** (commit `bbb9d23`).
F01 accepted implementation and validation revisions remain historical facts;
F02 adds the analysis foundation without changing F00/F01 frozen contracts.

F03 `src/opt/FlatPassManager` executes each registered named `FunctionPass` once
per non-declaration Function in pass-major / Module function order, without an
implicit fixpoint or Module/CGSCC/Loop adaptor. One Module-bound F02
`AnalysisManager` serves the pipeline; a fresh `IRMutator` and coupled
`CFGEditor` serve each invocation through `PassContext`. Only the tracked
`MutationSummary` determines changed; `NoChange`, `Declined` or `Failed`
with live mutation and `Applied` without mutation are contract violations that
stop the pipeline. `Failed` and `Declined` must be failure-atomic; pass failures
stop execution, clean declines continue. `PreservedAnalyses` (all/none/typed)
is declarative and never updates F02 cache state: CFG edits invalidate old
CFG-only Reachability handles; unrelated DataFlow edits do not. Optional
verify-each checks the entire Flat Module at entry and after every actual
invocation, and fails closed on verifier errors. Opt-in per-invocation records
carry stable pass/function identity, reported/final status, mutation summary,
verification, sorted statistics, and optional Flat dumps/timing. Pass-name
ablation records disabled/skipped distinctly from real `NoChange`. No target
cost service is introduced; a future target cost service must be immutable and
not an `AnalysisManager` analysis. F04 now routes Mem2Reg via formal
`Mem2RegPass : FunctionPass` and `FlatPassManager`, including the preserved
module compatibility API; per-candidate promotion uses the invocation's
`PassContext` and a fresh F02 Reachability handle. Candidates are aggregated
in Module Function order. The aggregate is pass-owned, so no external result
can dangle. Only module before/final after dumps and existing entry/output
verification remain enabled in production; per-invocation instrumentation is
optional. A failed pipeline reports `Mem2RegStatus::Failed` with
`success=false`, discards detached scratch, and the product path refuses to
emit an artifact; live commits are not rolled back, which is reachable only on
an internal invariant violation. **F04 CLOSED / PASS** (commit `10a3cbd6`;
independent review plus full S09 **55/0/2**, trajectories 357/62/0/121,
Mem2Reg structure 1/1/3 → 0/0/0+1φ, F04 unit **20/0**); DominatorTree remains
unmigrated.

## S12 interprocedural analyses (CLOSED / PASS)

`src/analysis/CallGraph.{h,cpp}` is a Module-scope analysis over non-declaration
Functions. Nodes keep direct call edges (sorted unique), caller edges, and
sorted external callee names (runtime library or unresolved declaration).
Edges index `CallGraphInfo::nodes`, which omits declarations. The shared
`classifyCallLike` recognizes ordinary calls and both specialized memory
intrinsic opcodes. The exact Flat names `llvm.memset` and `llvm.memcpy` are
external intrinsics in either representation; other spellings retain ordinary
call resolution and conservative external effects when unresolved.
Tarjan's iterative SCC pass emits components in bottom-up order; the analysis
fails closed if a node belongs to no/duplicate component or an inter-SCC edge
does not go from a higher slot to a lower one. The result records the Module
pointer and the Calls/Interface revisions at build time so a stale graph cannot
seed a CGSCC result.

`src/analysis/FunctionSummary.{h,cpp}` defines the first real CGSCC scope.
`AnalysisManager::get<SccSummaryAnalysis>(callGraph, scc)` keys the cache by
`(analysis, SCC slot, call-graph identity)`; the slot alone is not enough
because SCC numbering changes with the graph. Callee SCCs are fetched through
the manager, so their generations are recorded as dependencies. Purity is a
greatest fixed point: a member starts pure only with no local memory read/write
and no external call, and is removed when any callee is impure; mutual recursion
without effects therefore stays pure. Read/write/external effects are a least
fixed point, so recursion alone never invents a write. `readsMemory` and
`writesMemory` are independent conservative may-effect flags: memset writes,
memcpy reads and writes, and an unknown external call may do both. Effects
propagate through internal calls, including recursive SCCs. `pure` describes
these effects; it does not prove termination. The module-scope
`FunctionSummaryAnalysis` aggregates the SCC results and shares the same cache.

AnalysisManager freshness checks use a traversal-local active set to detect
cycles and a completed map to reuse validated `(key, generation)` dependencies.
Entry existence, generation and revision are checked before a completed result
is reused. A completed entry is recorded only after all dependencies validate;
the context is discarded after each top-level check, so mutations and lazy
recomputation cannot reuse an earlier validation. Each individual check expands
each dependency node once and examines its edges once, rather than enumerating
all paths through shared dependencies. This bounds a check by O(V+E); it does
not claim linear time for constructing all SCC summaries. The read-only
`freshnessExpansionCount()` is a cumulative diagnostic of actual expansions;
it does not participate in cache validity or invalidation decisions.

## S24b Order-preserving strip-mining (PARTIAL)

`LoopTilingPass` matches a canonical constant-trip loop (one i32 induction phi,
constant start/bound/positive step, one latch) and strip-mines it: a tile header
guards the original loop, the original loop runs `i` from the tile start to
`min(start+T, bound)`, and a tile latch advances by `T` (clamped to the bound to
avoid i32 overflow). The tile size comes from `planTiling`; because tile groups
contain consecutive iterations, the iteration order and side-effect sequence are
unchanged, so no dependence proof is needed. `CFGEditor::replaceTerminator`
accepts phi values for newly added edges, and all header rewrites are
prevalidated. Two-dimensional blocking (which reorders tiles) and fixed
matmul/conv/stencil runtime measurements remain S24c.

## S25a Unroll planning (CLOSED / PASS)

`planUnroll` combines target registers, live values, trip count and a
caller-supplied critical path. The factor is capped by the target (AArch64 up
to 4, RV64 up to 2), `registers/(live+1)`, the trip count, and
`bodyInstructions/criticalPath` when the path is known. AArch64 enumerates
power-of-two VF candidates jointly with UF (product at most eight); RV64 and
Generic stay scalar. Candidates are scored by `VF*UF/(1+penalty)` where the
penalty is `(live*UF/registers)^2`; candidates above the 0.8 cliff are
rejected. Cold/one-trip/empty loops are declined and an optional bounded probe
may accept or reject the chosen candidate.

`adviseUnrolls` consumes BlockFrequencyAnalysis and LoopInfoAnalysis: for each
canonical constant-trip loop it records trip count, body instruction count,
hotness and the plan result; non-constant or non-canonical loops are skipped.
There is no per-register-bank machine model, spill estimate or backend
calibration yet, and this layer does not rewrite the IR.

It does not rewrite the IR; S25b tracks the transform and runtime protocols.
The cap rejects `live >= registers` before adding the reserved register; for
UF >= 2 it bounds the projected-live multiplication by the register budget.

## S24a Cache-capacity tiling plan (CLOSED / PASS)

`planTiling` converts a nest's iteration count, dimensionality and element size
into a working-set estimate for two streams (read+write). The tile is the
largest power of two whose `2 * T^d * elementBytes` fits half of
`MemoryModel::cacheBytes`, bounded by the iteration count and a minimum of 8.
Full working sets that already fit, cold nests, undersized caches and empty
shapes are declined. An optional bounded `TileProbe` may accept or reject the
derived candidate. The planner does not rewrite loops; S24b tracks the tiled
IR transform and fixed matmul/conv/stencil measurement protocol.
Capacity decisions compare integer working sets, including exact-fit boundaries;
overflowing estimates saturate at UINT64_MAX, which exceeds the half-cache
budget. Candidate search takes at most the size_t bit width in steps; each
power calculation either returns immediately for extent=1 or saturates within
64 multiplications. No floating-point root determines feasibility.

## S23b Canonical loop interchange (CLOSED / PASS, load-only)

`LoopInterchangePass` matches a perfectly nested pair with exactly one i32
induction phi per header, constant start/bound/positive step, a single body and
a separate outer latch. Before editing it validates header sizes, predecessor
sets, phi/terminator shapes, in-region uses and that every body instruction is
pure or a load (no store/call/alloca). It then rewires all six edges with
`CFGEditor::redirectEdge`, passes the initial phi values to the swapped headers
and rewrites both latch increments to advance the new IVs. Unsupported shapes
are declined before mutation; an edit rejection after mutation fails the
pass/pipeline. General dependence-bearing bodies need affine extraction and are
not claimed.

## S23a Interchange planning (CLOSED / PASS)

`planInterchange` combines dependence legality with a cache-line cost model.
It accepts exact dependence distance vectors only; a pair is legal when every
vector is uniformly nonnegative or nonpositive, so swapping preserves the
dependence direction. A nest must be hot (>=0.5 of entry frequency) to be
profitable. `MemoryModel` converts a byte stride to cache lines per element;
the plan records old/new inner strides, line traffic, hotness and the reason.
The planner is a decision layer; the IR transform is S23b.

## S22a Tail-call classification (CLOSED / PASS)

`TailCallAnalysis` is a Function-scope consumer of `CallGraphAnalysis` and
`FunctionSummaryAnalysis` (dependency Calls|CFG|Memory). A call is a tail call
only when it is the operand of the block's `Ret`. Direct self calls are
`DirectSelfTail`; calls to a recursive callee are `MutualTail`; other/unknown
callees are `GeneralTail`, whose legality belongs to the backend. A direct
self tail call is stack-safe only when no argument transitively captures an
`Alloca`; the analysis returns per-call reasons and an all-safe flag. `TailRecursionEliminationPass` (S22b) consumes this analysis for a
single-block function whose only return is a direct self tail call: it stores
parameters into entry-block slots, clones the body into a new loop block that
loads the slots, stores the next arguments back and branches to itself, then
removes the call. Alloca/phi/multi-block/non-tail calls are declined, so the
transform stays within the stack-safety oracle.

## S21a Early inlining with bounded probe (CLOSED / PASS)

`InlineV0Pass` snapshots block frequencies and function summaries before its
first mutation (inlining invalidates the caller's analysis handles), then
accepts an optional `InlineProbe` with a per-run budget. A structurally
eligible call above the conservative size/cost cap may request a bounded
probe; a profitable result permits the inline, while an unprofitable
result or no probe keeps the call. Each decision appends an `InlineRecord`
(frequency, instruction count, code-growth and latency estimate, probe use,
reason: hot tiny / probe approved / probe declined / cost cap / cold).
Inlining only inserts into the caller block and rewrites the call, so an
enclosing loop's blocks and back edge remain intact.

`LoopRotatePass` rotates a phi-free while loop with one dedicated preheader,
one back edge, and phi-free body/exit targets: the header stays as a guard and
must contain only the condition (a single pure instruction whose operands are
outside the loop, used only by the terminator), so no header side effect is
bypassed; the latch gets a clone of that condition and its terminator becomes
`cond ? body : exit`. `CFGEditor::replaceTerminator` may now add phi-free target edges, not
only drop them. Phi-bearing loops are declined (negative test).

## S20 SCEV, LICM, strength reduction and AddressPlan (CLOSED / PASS)

`ScalarEvolutionAnalysis` maps canonical loop phis to `{start,+,step}` AddRec
expressions and folds constant add/mul derivatives. `LICMPass` hoists
side-effect-free invariant instructions to a preheader only when the header
already has exactly one outside predecessor dedicated to it; it never rewrites
the CFG, so no preheader means no hoist. Division/remainder, loads and calls
stay in the loop.

`StrengthReducePass` rewrites multiply by 2^k to `shl` and signed division by
2^k to the bias-and-arithmetic-shift sequence. Division by other constants uses
a 32-bit `mulhi` (16-bit limb emulation, since Flat integer ops are i32-only),
the usual magic reciprocal, a remainder correction and an INT_MIN select; the
unit test evaluates the transformed IR against C++ division across seven
divisors and thousands of values. `AddressPlan` compares indexed and
pointer-induction latencies from a `CostVector`; without target facts it falls
back to indexed addressing, and two legal target cost vectors can select
opposite strategies. The plan is a decision API, not yet an IR rewrite.

## S19 GVN, DSE and DLE (CLOSED / PASS)

`GVNPass` performs dominator-scoped value numbering for pure instructions
(arithmetic, comparisons, select, casts, GEP). A scoped availability map is
entered on the dominator-tree DFS and unwound on block exit, so a duplicate is
replaced only by a value from a dominating block; sibling branches never merge.
Commutative integer operations canonicalize their operand order. Loads and
calls are not numbered.

`DeadStoreEliminationPass` scans a block backwards and removes a store
overwritten by a later store to the same non-escaping stack-slot pointer. Any
load or call-like operation clears all pending stores; global/argument/unknown
pointers also flush them, because the project's equivalence oracle observes
global write traces. It does not cross block boundaries. `DeadLoadEliminationPass` removes unused loads.
`--opt=s19` is an opt-in pipeline (rewrite, GVN, DSE, DLE) after mem2reg.

## S18 rewrite DSL and basic peephole rules (CLOSED / PASS)

`src/opt/RewriteEngine.{h,cpp}` separates matching from rewriting. A `Pattern`
matches an opcode, exact operand count, optional integer result type, and
operand patterns (`Any`, `Constant`, `ConstantInt`, `Capture`, `SameAs`);
captures are name-keyed and repeated captures must bind the same value.
`RewriteRule` adds an `onlyIf` guard evaluated after matching and a builder
that returns the replacement value. The engine applies the first matching,
guard-passing rule through `IRMutator::replaceAndErase`; guarded and declined
attempts are counted by rule.

`installBasicRules` covers 20 production peepholes: additive/multiplicative
identities, `x-x`, duplicate `select` arms, self-comparison, and bitwise/shift
identities. `RewritePass` applies them to a fixed point. The pass is not wired
into the default pipeline yet; S19 builds the value-numbering layer on it.

## S17 constant propagation, DCE and CFG simplification (CLOSED / PASS)

`ConstantPropagationPass` folds pure i32/i64 arithmetic, comparisons, select,
extensions and truncations with canonical constants (signed overflow wraps;
division by zero is not folded). `DeadCodeEliminationPass` removes unused
pure instructions and unused phis through `IRMutator`/`CFGEditor`.
`SimplifyCFGPass` folds a conditional branch with a constant condition or two
identical targets into an unconditional branch. It uses the new
`CFGEditor::replaceTerminator`, which requires the new successor set to be a
subset of the old one and refuses to drop a predecessor from a phi-bearing
target. The pass additionally refuses to drop the last predecessor of the
unchosen successor, because that would make it unreachable and invalidate
phi dominance downstream. `--opt=s17` is an explicit opt-in pipeline; default
O1 remains mem2reg only.

## S16b foundation audit (CLOSED / PASS)

`tools/selftest/check_foundation_audit.py` enforces the structural controls:
`DominatorTree` appears only in the shared `ir` implementation and the
dominator analysis wrapper, `src/opt`/`src/analysis` do not call
`rebuildCFG/rebuildUseDef`, `src/ir` does not include `analysis/`, and every
analysis header declares its scope and revision dependencies.
`test_foundation_audit` exercises the DataFlow/CFG/Memory/Calls invalidation
matrix across the S12-S16 analyses and lazy recompute. The dated audit report
under workspace `docs/audit/` records residual, non-blocking findings.

## S16 affine dependence analysis (CLOSED / PASS)

`src/analysis/DependenceAnalysis.{h,cpp}` models an access as one affine
subscript per loop dimension (`coeff * iv + constant`) with a bounded iteration
domain. `testDependence` runs GCD divisibility first, then a Banerjee range
test over the bounds, and returns an exact sink-minus-source distance vector
only when every dimension has a common coefficient. Different coefficients can
still be dependent but keep `distance` empty (`Possible`); any dimension with
no integer solution, or a Banerjee range excluding zero, proves independence.
Constant-only dimensions compare their constants and contribute distance 0 when
equal. Malformed dimension counts fail closed.

## S15 cost protocol and inlining V0 (CLOSED / PASS)

`src/opt/CostModel.h` defines an immutable `TargetCostModel` service and a
`CostContext` with confidence; it is deliberately not an AnalysisManager
analysis, so target facts are not invalidated by IR mutations. The
`ConservativeCostModel` charges a fixed call overhead, one unit per
instruction, and allows at most two instructions without target facts (eight
when facts exist).

`InlineV0Pass` is a FunctionPass that consumes `BlockFrequencyAnalysis` and
`FunctionSummaryAnalysis`. It only clones a callee that is single-block,
non-recursive, free of phi/alloca, hot (call-block frequency at least half of
the entry), and within the model's size and cost caps. Cloned operands map
arguments to call operands and instruction results to clones; the call is
replaced by the returned value or erased for void. Every declined site is
counted. The pass is not part of the default pipeline and must prove measured
benefit before enabling.

## S14b branch probabilities, frequencies and value facts (CLOSED / PASS)

`BranchProbabilityAnalysis` is a Function-scope consumer of LoopInfoAnalysis.
A loop header assigns 0.9 to successors inside the loop and 0.1 outside; a
non-header branch whose successor is a loop header treats that edge as a rare
back edge (0.1) and the others as 0.9; other branches split evenly. The
probability source is therefore explainable from loop structure, not a hidden
model.

`BlockFrequencyAnalysis` consumes the probabilities and solves block flow with
a bounded relaxation (entry 1e6, 4*blocks+32 rounds, values capped at 1e12).
`ValueFactsAnalysis` is a conservative integer range analysis over
constants/add/sub/mul/sdiv/srem/icmp/select/phi; i32 overflow, division by an
unknown/zero divisor, loads, calls and casts stay unknown. Both invalidate on
the revision facts they consume (CFG; DataFlow|CFG for value facts).

## S14 CFG, dominators and loops (CLOSED / PASS)

`src/analysis/DominatorAnalysis.{h,cpp}` is a Function-scope wrapper over the
one `ir::DominatorTree` implementation shared with `FlatVerifier`; it copies the
immutable idom/reachability/postorder facts and does not contain a second
dominator algorithm. `src/analysis/LoopAnalysis.{h,cpp}` derives natural loops
from dominator back edges: each header dominates its back-edge tails, reverse
predecessor walks stop at the header, multiple tails merge into one loop, and
block sets are sorted. Loops are ordered by header; `parent` is the smallest
enclosing loop and `depth` follows the parent chain. Non-natural cycles (no
header dominating its tail) produce no loop.

The AnalysisManager's new Loop scope keys `get<LoopViewAnalysis>(function,
loopIndex)` by Function and loop slot; freshness follows the CFG revision.
Out-of-range slots and stale CFGs fail closed.

## S13 alias analysis (CLOSED / PASS)

`src/analysis/AliasAnalysis.{h,cpp}` is a Module-scope analysis with the
dependency mask DataFlow|Memory|Calls|Globals|Interface. It consumes CallGraphAnalysis
and keeps an immutable provenance map in its result: Alloca, Global, Param and
Fresh bases, with constant byte offsets accumulated through GEP, BitCast and
Phi. Distinct Alloca/Global/Fresh objects and distinct constant offsets of the
same base are NoAlias; equal base and offset are MustAlias; anything unknown is
MayAlias.

Function return provenance is a bounded fixpoint over the call graph. A return
of a parameter or global is exact only while no non-zero/dynamic GEP moved it;
call sites substitute the actual argument for a parameter return. Interior
returns keep an unknown offset, and unknown/external results stay MayAlias, so
the analysis never proves NoAlias from missing information. The alias result is
itself an AnalysisManager entry; it does not keep mutable cross-revision state.

## F05 CompilationSession and CMake components (CLOSED / PASS)

Flat text value ids are translated only through the pre-scanned definition
table. An unregistered id is a diagnostic, never a fallback to the internal
number; a legal forward reference (definition in a dominating block that is
textually later) is created with a placeholder operand and bound at function
end, followed by a per-function unresolved-id check. `dumpModule` output must
round-trip byte-identically. Value ID zero is valid, including a forward call
argument; parsing tracks whether an ID exists separately from its number.

`src/main/CompilationSession` is the only stage materialization path. It lazily
builds AST → Sema → InitPlan → Structured → Flat, caches each stage, and keeps
`--from-ast` / `--from-structured` / `--from-flat` as input modes of the same
session. `main.cpp` parses the command line, chooses the artifact and writes
it; it no longer contains lexer/parser/sema/flatten calls.

`CMakeLists.txt` defines static component libraries `sysy_support`,
`sysy_frontend`, `sysy_ir`, `sysy_analysis`, `sysy_opt` and `sysy_structured`
whose target dependencies encode the allowed include direction. Unit tests
link those targets and are registered in CTest; `tools/selftest/unit/run.sh`
only builds and runs them (including the `compiler` target used by the CLI
round-trip), so adding a source file no longer requires editing a second
source list and a missing binary is a failure, not a skip. `check_f05_session_boundary.py` enforces both invariants
with negative controls. Full S09 gate after F05: **56 PASS / 0 FAIL / 2 expected SKIP**.

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

The Flat optimization contracts (F01 mutation, F02 cache, F03 manager present) are:

- every `Value` kind owns precise reverse `Use{user, operandIndex}` edges; a
  repeated operand is multiple uses even when it has one distinct user;
- optimization code mutates operands/instruction membership only through an
  `IRMutator`, and CFG/phi structure only through a `CFGEditor`;
- arena/central ownership is allowed for pointer stability, but owned objects
  and live IR membership are distinct; erased/tombstoned instructions are not
  traversed by normal analyses;
- F01 records data-flow/CFG/memory/call revisions (Globals/Interface
  fields reserved); F02 validates caches against declared revision categories
  and dependencies; F03 `PreservedAnalyses` cannot override
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
revision-aware `AnalysisManager`; F03 now implements `FlatPassManager` before S12.
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

## F02 AnalysisManager foundation (CLOSED / PASS)

F02 implementation and repair chain:
`459fe54f862bacd7974fa734e01e37f6982f96b2` (initial AnalysisManager)
→ `923482ebf62236a3c00260836048f7de1ff3e591` (Fix1 ownership/value
boundary) → `59885f9d29d9df3967cc5488cff997c68f0c7a60` (Fix2 BasicBlock
Module ownership) → `7fdb9f33f254bc0219269cafdc8e0b5e36d2e6a9`
(Fix3 verifier classification contract). Fix1 gives every Flat Value
immutable Module provenance, Function-local Instruction/Argument checks,
and fail-closed atomic mutator/CFG preflight. Fix2 separates BasicBlock
Module lifetime ownership from Function membership and makes the verifier
audit Module block inventories and duplicate/orphan membership. Fix3 corrects
pure `block_bad_index` to V4 without mislabeling it V1; true ownership
negatives remain V1.

An `AnalysisManager` binds one `flat::Module` for its entire lifetime;
it owns Module and Function cache entries with type-index analysis identity.
Function requests reject foreign parent modules. Typed handles hold immutable
shared results and observe manager lifetime through a weak token, returning a
`const Result*` only for the current generation. Manager destruction and
stale generations cause `get() == nullptr` without dangling pointers.

Each analysis declares a mask over the six F01 revision categories: DataFlow,
CFG, Memory, Calls, Globals and Interface (None means no direct IR
dependencies). Function entries observe Function revisions; Module entries
observe Module revisions. Changes to unrelated categories or other Functions
preserve compatible caches. While computing, the manager automatically tracks
requested analysis identities and generations. Recursive dependency freshness
catches transitive invalidation even for parents declaring None; cycles,
failed dependencies and failed computations never cache partial results.
Revision mismatches invalidate entries; the next request lazily recomputes.

`ReachabilityAnalysis` is a CFG-only immutable Function result, using the
original entry-based terminator-successor traversal. Mem2Reg consumes it per
candidate before mutation and conservatively declines if unavailable, while
`DominatorTree` remains a direct legacy construction (not manager-migrated).
At the historical F02 acceptance revision, no mutation callbacks or F03
`FlatPassManager`, `PreservedAnalyses`, `PassContext` or `FunctionPass` existed.
F03 was later accepted separately on 014 evidence and closed by its own
implementation/documentation commits; this does not retroactively change the
frozen F02 acceptance evidence.

F02 acceptance: valid final 004 on unchanged clean
`7fdb9f33f254bc0219269cafdc8e0b5e36d2e6a9` HEAD/fingerprint;
unit **PASS** with `VALUE_USE_FAILURES=0` and
`ANALYSIS_MANAGER_FAILURES=0`; S08 **49 cases / 0 failures**
(44 invalid / 7 positive / 8 legacy, index V4 and ownership V1);
complete S09 **54 PASS / 0 FAIL / 2 expected SKIP**
(`S08_OK=YES`, `MEM2REG_OK=YES`); trajectories **357 exact /
62 loop-count-only / 0 observable differences / 121 categorized skips**;
LLVM **490 supported PASS / 50 tensor expected reject / 0 failure /
0 timeout**. Full O1 Flat was **490/490 byte-identical**, 0 mismatch,
with **50/50** tensor rejects on both baseline and current. Frozen pre-F02
compiler SHA256 is
`15235a6fa38026df9b06dd21750f19e029b4c263dd97955a59c6754333cd7d06`;
manifest SHA256 is
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.
Ownership regression fail-closed and architecture, mutation-boundary,
line-budget, CLI, syntax, git-diff gates **PASS**. Independent web review
008 checked the original evidence: **PASS / 0 findings**. Attempts
001/002/003 were environment/working-directory **INVALID_TEST_STATE**,
not product failures or accepted proof.

**REUSED FROM 59885f9 VALIDATION** (not rerun on Fix3): full x86 O1
**490/50/0/0**, full no-structured O1 **490/50/0/0**
(PASS/SKIP/FAIL/TIMEOUT); focused O0 normal **3/3**, O0 no-structured
**3/3**, AArch64 O1 **3/3**, RV64 O1 **3/3**, toy O1 **3/3**.
Exact diff `59885f9..7fdb9f3` touches only the verifier classification
and corresponding unit test, so these target results remain explicitly
reused evidence, not newly executed runs.

## F03 FlatPassManager foundation (CLOSED / PASS)

F03 implementation commit:
`b1c06bb936049fc713c0db16eef8015b7b3ab2ec`
(`feat: add F03 flat pass manager foundation`), with parent/base
`3f824833cb4d403c926f9f9f0ed2f2dd3a529e0a`. Implementation scope was
exactly four modified tracked files (`CMakeLists.txt`,
`docs/ARCHITECTURE.md`, `docs/CURRENT-STATE.md`,
`tools/selftest/unit/run.sh`) plus three newly tracked files
(`src/opt/FlatPassManager.cpp`, `src/opt/FlatPassManager.h`,
`tools/selftest/unit/test_pass_manager.cpp`). F03 itself has **no Mem2Reg or
DominatorTree migration**; F04 later migrated Mem2Reg as recorded above, and
DominatorTree remains unmigrated in `FlatVerifier`. No ModulePass,
CGSCC/Loop adaptor, CallGraph, FunctionSummary, AliasAnalysis, LoopInfo,
SCEV, target-cost, CompilationSession or backend exists from this stage;
F05 and S12+ have not started.

Each registered FunctionPass runs once per non-declaration Function in
pass-major / Module Function order; no implicit fixpoint. The pipeline shares
one Module-bound F02 AnalysisManager, while each actual invocation obtains
fresh IRMutator, CFGEditor and PassContext. Changed/MutationSummary reflect
only applied tracked live mutations, not pass-reported claims:
NoChange/Declined/Failed + mutation and Applied + no mutation are
fail-closed contract violations. F01 revisions and F02 dependency/cache
invalidation remain authoritative over declarative PreservedAnalyses.
Verify-each checks the Module at pipeline entry and after every actual
invocation, including failure-atomic checks. Stable pass-name disable/ablation,
optional before/after dump, per-invocation timing, deterministic ordered
statistics and instrumentation are supported.

014 candidate identity was **two-part**, not a whole-seven-file diff:
the four originally tracked modifications yielded
`git diff --binary HEAD -- <four tracked paths>` SHA256
`ea5cc3da5de339310bfbe2800db2c6fa5abda7d654c3a11d7244cb201edf7d51`
and byte-exactly matched 014 `before.diff`. The three then-untracked file
SHA256 identities were `FlatPassManager.cpp`
`29ac6199cc86de1e1335a7687d2df8cc9c38f91ae9579df7e8e934e9b55b05d6`,
`FlatPassManager.h`
`dc8a5c7a1540272fb8d20a421f063ae927c70e0edf292670ceb78ef97bf5652d`,
and `test_pass_manager.cpp`
`ac60e1abede1a2d26a8fcf8856603a16fe5dc4d446362c19bcce65509fc3fffe`.
The 014 before-fingerprint
`5c23f31c701cbc5435a67a8ed57c2411bdbe576445dbcebe5ae6907dde7c6a4b`
is historical evidence only. The implementation commit's full patch includes
the three additions and therefore need **not** equal the tracked-only patch:
post-commit equivalence was established by four-file patch equality and
all three committed new-file blob content SHA256 values.

014 terminal acceptance: formal S09 **exit=0, 54 PASS / 0 FAIL /
2 expected SKIP**; S08 public matrix **44 invalid / 7 positive / 8 legacy**
and S08/Mem2Reg gates **PASS**; trajectories **357 exact /
62 loop-count-only / 0 observable differences / 121 categorized skips**;
LLVM **540 total / 490 supported PASS / 50 tensor frontend reject /
0 FAIL / 0 TIMEOUT**; `test_pass_manager` **41 checks /
8 negative controls / 0 failures**, unit runner **exit=0**.
Full O1 Flat protocol: `IDENTITIES=540`, `SUPPORTED=490`,
`BASELINE_TENSOR_REJECT=50`, `CURRENT_TENSOR_REJECT=50`,
`MISMATCH=0`, `ERRORS=0`, `BYTE_IDENTICAL=490`, **exit=0**.
Manifest SHA256
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.
014 confirmed unchanged HEAD/status/candidate identity across validation;
fresh independent web review was **PASS / 0 findings**.
011/012 produced no terminal long-validation evidence (not compiler
failures); 013 was superseded by 014 and never started a WSL gate;
015/016 were closeout identity-rule mistakes, made no project changes and
are neither compiler failures nor acceptance evidence. 017 commits the
unchanged validated implementation and closes documentation only; it does
not rerun full S09/540/target matrices or start F04. Post-closeout
independent review remains required.

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
