# Compiler current state

This is the compact current-state authority for the standalone compiler. A
handoff or audit may preserve older findings, but its numbers are historical
unless it links a new run.

## Implemented

- C++17 build and command-line front end.
- Lexer, parser, AST, semantic checks, constant evaluation, and initialization
  planning for the current SysY2022 scope.
- Structured IR and reader/verifier, loop normalization, and structured dumps.
- Flattening into the flat CFG/SSA-oriented IR and flat dumps.
- Runtime signature catalog and source location tracking.
- `IREmitter` and `LLVMTextEmitter`, which render verified flat modules as
  target-neutral opaque-pointer LLVM IR text.

The default product path now verifies the structured and flat modules before
emitting LLVM IR; `--from-flat` accepts a serialized flat module. Unsupported
emission fails without creating or overwriting the output file. The text is
target-independent and does not include triple or data layout. `-O1` / `--optimize`
explicitly enable the current O1 pipeline (today: mem2reg), while the default
product path remains O0. `--mem2reg` stays an explicit focused switch for tests
and diagnostics.

## Next milestone

**S11 closed** on clean compiler revision
`52018acfa087ec8bc97ae20d4570290857c2cd3b`, after final independent clean
revalidation. Its clean S09 prerequisite on the same revision passed 52 checks,
0 failed, with 2 expected skips: 490 supported cases and 50 tensor expected
rejects; 357 exact trajectories, 62 loop-count-only, 0 observable differences,
and 121 categorized skips. FlatVerifier reported 44 invalid, 7 positive, and 8
legacy cases. Mem2Reg changed the fixture from 1 alloca / 1 load / 3 stores to
0 / 0 / 0 and 1 phi; the forged-stat negative control, S08_OK, and MEM2REG_OK
all passed.

Four revision-bound S11 evidence reports each passed 490 supported cases, skipped
the 50 expected tensor cases, and had 0 failures and 0 timeouts: x86,
AArch64, RV64, and no-structured. All report schema version 1, compiler revision
`52018acfa087ec8bc97ae20d4570290857c2cd3b`, and `compiler.dirty = false`. The
compiler artifact `/home/koki1/try/compiler/build/compiler` has SHA256
`7e771737703d8f3e685c9dbe15aa3478c9fc88182ffaa33ad6670ef2e5acf8c3`; the
manifest `/home/koki1/try/tests/manifest.tsv` has SHA256
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.

The no-structured evidence follows the real boundary: source → `--emit=flat-ir`
→ serialized Flat → fresh compiler process → `--from-flat --emit=llvm-ir` →
LLVM target lowering → execution (`mode.no_structured = true`,
`mode.boundary = from-flat`). S11 target timeouts are x86 native 60 seconds per
case (including no-structured x86), AArch64 QEMU 300, and RV64 QEMU 300; these
are correctness hang limits, not performance thresholds. AArch64 and RV64
evidence uses QEMU 8.2.2 proxy metrics (`metric_scope = qemu-proxy`, triple
`aarch64-linux-gnu` and `riscv64-linux-gnu`, respectively; RV64 dumpmachine is
`riscv64-linux-gnu`, with `rv64imafdc_zicsr_zifencei` / `lp64d`). It is not real
target performance: there are no real Cortex-A53 or BOOM v3 measurements.

The S11 measurement protocol supports warmup, repeats, raw samples, median,
MAD, comparison reports, per-case speedup, geomean, and worst/best case.
Focused smoke proves that this plumbing works; its two O0/O1 cases are not
optimization-gain evidence. S11 also fixed the `verify.sh /tmp/verify_$$`
workspace collision by using `mktemp -d .../sysy_verify.XXXXXXXX`, and raised
QEMU correctness timeouts to 300 seconds to avoid killing valid long cases.

S11b is closed on revision
`7648f928fcc4cd45dad66a2b97ce5bfe22875a01`. Its independent long validation
reported 490 required PASS, 50 tensor SKIP, 0 FAIL, 0 TIMEOUT, with
`format_diff = 127`; those format-difference cases passed the frozen C1–C3
output-matching contract. The 600-second per-case timeout was a correctness
hang limit, not a performance threshold.

The self-built toy lowering provider was audited on all 490 required rows:
490 toy lower invocations, 0 llc, 0 clang/gcc lowering, and 0 compiler
invocations containing `--toy-backend`. The no-LLVM focused smoke passed, as
did fake-llc, wrong-provider-path, and wrong-provider-hash negative controls.
S11b made no `src/**` changes. `build/compiler` remains byte-identical to the
frozen S11 binary (SHA256
`7e771737703d8f3e685c9dbe15aa3478c9fc88182ffaa33ad6670ef2e5acf8c3`); the
manifest SHA256 is
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.
The toy backend is a correctness-only protocol consumer, not the formal
backend and not a performance backend.

F00 is **CLOSED / PASS** on accepted compiler revision
`f9425dc56b74b9312f6a336f334e5b347844e830`: first-class Value/Use,
independent Argument Values, canonical Constants, and frozen Flat text protocol.

F01 is **CLOSED / PASS** on accepted validation revision
`22eb7a66d6eb57073ecf904323cf10212fe2c247` (implementation/Fix1:
`5f4173caeb8abf78d1ac56c129c9550985f48b0b`; the only difference is
`README.md`). IRMutator/CFGEditor now own tracked optimizer and CFG changes,
with Detached/Live/Tombstone ownership semantics, Module/Function revisions
and MutationSummary; existing Mem2Reg consumes both editors. Acceptance:
O1 Flat **490/490 byte-identical** (50/50 tensor rejects unchanged);
current F01 S09 **54 PASS / 0 FAIL / 2 SKIP**, **0 observable differences**;
full x86 O1 **490/50/0/0** and no-structured O1 **490/50/0/0**
(PASS/SKIP/FAIL/TIMEOUT). The historical pre-F01 S09 total was 52/0/2;
the two new architecture boundary gates account for 54.

**F02 CLOSED / PASS** on accepted compiler revision
`7fdb9f33f254bc0219269cafdc8e0b5e36d2e6a9`, based on valid final
validation 004 and independent evidence closeout 008. The implementation/fix
chain, in order, is: initial AnalysisManager
`459fe54f862bacd7974fa734e01e37f6982f96b2` → Fix1 ownership/value
boundary `923482ebf62236a3c00260836048f7de1ff3e591` → Fix2 BasicBlock
Module ownership `59885f9d29d9df3967cc5488cff997c68f0c7a60` → Fix3
verifier classification contract `7fdb9f33f254bc0219269cafdc8e0b5e36d2e6a9`.
Fix1 enforces Instruction/Value provenance, Function-local SSA/Arguments,
and atomic fail-closed mutator/CFG edits; Fix2 enforces immutable BasicBlock
Module provenance, registered Function membership, and independent inventory
verification. Fix3 classifies pure `block_bad_index` as V4 rather than V1
(ownership negatives remain V1).

The final F02 `AnalysisManager` is Module-bound and revision-aware: it caches
typed immutable Module/Function results, checks masks over all six revision
categories (DataFlow, CFG, Memory, Calls, Globals, Interface), automatically
tracks analysis dependency identities/generations including transitive
dependencies, fails closed on cycles, failed dependencies, foreign Functions
and stale handles, and lazily recomputes invalidated entries.
`ReachabilityAnalysis` is CFG-only at Function scope; Mem2Reg consumes it per
candidate and requests a fresh handle after every live mutation. `DominatorTree`
has no `src/opt/` consumer: it remains a direct construction inside
`FlatVerifier` and its tests on the legacy path.
**F03 CLOSED / PASS** on final validation 014 and its independently
reviewed evidence (review **PASS / 0 findings**). The validated implementation
was committed unchanged as `b1c06bb936049fc713c0db16eef8015b7b3ab2ec`
(`feat: add F03 flat pass manager foundation`), parent
`3f824833cb4d403c926f9f9f0ed2f2dd3a529e0a`. **F04 CLOSED / PASS** on
accepted commit `10a3cbd6` (`refactor: migrate Mem2Reg to the formal
FunctionPass`) plus an independent review that found one blocker on the
failure path (fixed before acceptance) and the full S09 gate: production and
compatibility Mem2Reg now use the first formal FunctionPass (`Mem2RegPass`),
the module API is a FlatPassManager wrapper, the aggregate is pass-owned (no
external pointer), detached scratch phis are discarded on a thrown plan, and
a failed pipeline reports `Mem2RegStatus::Failed` with `success=false` and no
artifact. DominatorTree remains unmigrated in `FlatVerifier`; no pass consumes
it. **F05 CLOSED / PASS** (commit `bbb9d23`; full S09 gate **56 PASS / 0 FAIL /
2 expected SKIP**, unit suite **20 tests via CTest**, F05 boundary checker
**PASS**): `main.cpp` is CLI/artifact policy only, `CompilationSession` owns
the single AST→Sema→InitPlan→Structured→Flat materialization path, CMake
defines `sysy_*` component libraries, and unit tests link those targets
instead of re-copying source lists. **S12 CLOSED / PASS** (2026-10-19; full S09 **56/0/2**,
`test_interprocedural` **33 checks**): `CallGraphAnalysis` builds the module call
graph with SCC condensation in bottom-up order, external-call tracking, and
call/interface provenance; `SccSummaryAnalysis` is the first real CGSCC scope
(cache keyed by call-graph identity and SCC slot); `FunctionSummaryAnalysis`
aggregates it. Purity is a greatest fixed point over the SCC (no load/store,
no external call, all callees pure), so mutual recursion without effects stays
pure; read/write/external effects are a least fixed point. Dependencies:
`Calls|Interface` for the call graph, `Calls|Memory|Interface` for summaries.
`test_interprocedural` covers SCC order, recursive purity, effect propagation,
cache reuse, revision invalidation and stale-graph rejection (33 checks).
F00/F01/F02/F03/F04/F05 frozen acceptance remains unchanged; **S13 is next**.

**S20a CLOSED / PASS** (full S09 **57/0/2**, `test_s20` **11 checks**): `ScalarEvolutionAnalysis`
recognizes canonical induction-variable phis (`{start,+,step}`) and add/mul
derived expressions; `LICMPass` hoists pure invariant instructions to a
dedicated preheader (no CFG rewriting, so no preheader means no hoist);
`StrengthReducePass` lowers power-of-two signed division to shift/bias and
multiply-by-power-of-two to `shl`; `AddressPlan` chooses indexed vs pointer
induction from `CostVector` facts, falling back to indexed without target
facts. `test_s20` covers SCEV, LICM (positive, IV-negative), strength
reduction and both AddressPlan choices (11 checks). Transpose 220 → 202
instructions; S20 corpus behavior 357 exact + 62 loop-count-only + 0
observable differences + 121 skips, all pass.

**S21a CLOSED / PASS** (full S09 **57/0/2**, `test_inline_v0` **14 checks**):
`InlineV0Pass` now takes an optional bounded `InlineProbe`. Calls that pass the
structural checks but exceed the conservative cost cap may request one of at
most `probeBudget` probes; a profitable probe inlines, an unprofitable or
absent probe declines. Every decision is recorded as an `InlineRecord` with
frequency, instruction count, code-growth/latency estimate, probe use and
reason. Tests cover probe-approved, probe-declined, no-probe fallback, records
and inlining inside a loop while preserving its body/back edge.

**S22a CLOSED / PASS** (full S09 **57/0/2**, `test_tailcall` **6 checks**):
`TailCallAnalysis` classifies every call as not-tail, direct-self-tail,
mutual-tail or general-tail (the latter two are explicitly backend decisions).
A direct self tail call is stack-safe only when no argument may capture a
stack object (transitively via alloca); the result exposes per-call reasons and
an all-safe flag. `test_tailcall` covers direct/general, non-tail result use,
mutual recursion and alloca-capturing arguments.

**S23a CLOSED / PASS** (full S09 **57/0/2**, `test_interchange` **7 checks**):
`planInterchange` decides adjacent loop interchange legality from exact
dependence distance vectors (uniform nonnegative/nonpositive directions),
requires a hot nest, and compares cache-line traffic before/after with a
`MemoryModel` (64/128-byte lines). The plan records the outer/inner indices,
strides, traffic, hotness and reason; a target line size can change the
benefit decision. The test includes a hand-checked equivalence oracle over
legal/illegal directions.

**S23b CLOSED / PASS (canonical two-level, load-only body)**:
`LoopInterchangePass` recognizes a canonical perfectly nested pair (outer
header -> inner header -> single body; inner exit increments the outer IV) and
swaps the loop roles: headers exchange incoming phis, the new inner exit and
new outer latch receive swapped increments, and all six edges are redirected
through `CFGEditor::redirectEdge` with phi values. The body must contain no
store/call/alloca, so the reorder is dependence-free; the planner still records
legality and line-traffic estimates. `test_interchange_transform` covers the
swap, a no-phi loop (no crash), header-side-effect and store negatives (12
checks). The matcher validates induction phis, constant domains, predecessor
sets, in-region uses and edit failure before/after mutation. Runtime measurements are not claimed.

**S22b CLOSED / PASS (single-block scope)**: `TailRecursionEliminationPass`
consumes `TailCallAnalysis` and transforms a one-block function whose only
return is a direct self tail call. Parameters are stored in entry-block slots;
a new loop block loads them, clones the body, stores the next arguments back
to the slots and branches to itself, so the recursive call disappears and the
frame does not grow. Functions with an alloca, phi, multiple blocks or a
non-tail call are declined. `test_tco` covers the transform, entry/loop CFG,
and both negatives (5 checks).

**S21b CLOSED / PASS for phi-free loops**: `CFGEditor::replaceTerminator`
can now add phi-free successor edges as well as drop them.
`LoopRotatePass` rotates a while loop with a dedicated preheader, one back
edge, no header phi and phi-free body/exit targets: it keeps the header as a
guard and requires it to contain only the condition (single pure instruction,
used only by the terminator, operands outside the loop); the latch receives a
clone of that condition and its terminator becomes `cond ? body : exit`. It
does not rotate phi-bearing loops yet, and header side effects block rotation;
`test_looprotate` covers rotation, back-edge removal, the phi negative and a
header-side-effect negative (7 checks).

**S20b CLOSED / PASS**: signed division by a non-power-of-two constant is
now lowered with a 32-bit `mulhi` (16-bit limb emulation; Flat integer ops are
i32-only), the magic reciprocal, a one-step remainder correction and an
INT_MIN select. `test_s20` evaluates the transformed IR against C++ truncating
division for 7 divisors and ~2,000 values each (including INT_MIN), and checks
two legal target cost vectors choose opposite address strategies. Behavior
with `--mem2reg --opt=s20`: 540 files, 357 exact + 62 loop-count-only + 0
observable differences + 121 skips, all pass. AddressPlan remains a decision
API (indexed/pointer) rather than an IR rewrite.

**S19 CLOSED / PASS** (full S09 **57/0/2**, `test_s19` **7 checks**):
`GVNPass` numbers pure instructions in dominator-tree scope (scoped
availability, commutative operand canonicalization, no cross-sibling merge);
`DeadStoreEliminationPass` removes an overwritten store to the same
non-escaping stack slot in one block; loads/calls and non-alloca pointers flush
pending stores (global write traces stay observable); `DeadLoadEliminationPass` removes
unused loads. `--opt=s19` runs rewrite → GVN → DSE → DLE after mem2reg.
`test_s19` covers same-block GVN, sibling dominance, commutation, DSE with an
intervening load, the alloca-only DSE scope and DLE (7 checks). Transpose: 220 → 214 instructions with
S19, 220 → 191 with S17+S19. Behavior with `--mem2reg --opt=s17 --opt=s19`: 540 files, 357 exact + 62 loop-count-only + 0 observable differences + 121 skips, all pass.

**S18 CLOSED / PASS** (full S09 **57/0/2**, `test_rewrite` **24 checks**):
`opt/RewriteEngine` is a small peephole DSL: `Pattern` matches opcode,
operand count, integer result and operand patterns (Any/Constant/ConstantInt/
Capture/SameAs); `onlyIf` guards run after matching and are counted when they
block a rule; builders return a replacement value. `installBasicRules` provides
20 rules (add/sub/mul identities, x-x, select duplicate arms, self-comparison,
bitwise/shift identities) and `RewritePass` applies them to a fixed point.
`test_rewrite` exercises every rule, the guard negative control and the fixed
point (24 checks).

**S17 CLOSED / PASS**: `ConstantPropagationPass` folds i32/i64
add/sub/mul/div/rem/shifts/bitwise/icmp/select/ext/trunc, `SimplifyCFGPass`
folds constant conditional branches and duplicate targets through the new
`CFGEditor::replaceTerminator` (dropping a predecessor is refused when it
would make the other successor unreachable), and `DeadCodeEliminationPass`
removes unused pure instructions and phis. `--opt=s17` runs the three passes
after mem2reg; the pass set is opt-in, not part of default O1.

Evidence: 490 supported files measured with `--mem2reg` vs `--mem2reg
--opt=s17`: **244,649 → 217,027 instructions (−27,622, −11.3%)**, 378 cases
reduced, 0 increased. Corpus behavior with `--mem2reg --opt=s17`: 540 files,
**357 exact + 62 loop-count-only + 0 observable differences + 121 skips**,
`全过`. `test_s17` covers folding, DCE, constant/duplicate branch folding, a
phi-bearing join and the last-predecessor refusal (8 checks).

**S16b CLOSED / PASS** (full S09 **57 PASS / 0 FAIL / 2 expected SKIP**):
the foundation audit adds `check_foundation_audit.py` (single dominator
implementation, no manual rebuilds in analyses/passes, no `ir -> analysis`
includes, every analysis declares scope/dependencies) and
`test_foundation_audit` (14-check revision/invalidation matrix). Residual
non-blocking findings and follow-up triggers are recorded in
[`../docs/audit/S16B-FOUNDATION-AUDIT-2026-10-20.md`](../../docs/audit/S16B-FOUNDATION-AUDIT-2026-10-20.md).

**S16 CLOSED / PASS** (full S09 **56/0/2**, `test_dependence` **28 checks**):
`analysis/DependenceAnalysis` implements the affine dependence pipeline over
one loop nest: GCD divisibility, a bounded Banerjee range test, and an exact
distance vector when every dimension has a common coefficient. Independent
dimensions fail closed; differing coefficients return `Possible` without a
distance. `test_dependence` covers 13 hand-computed cases (distances 0/±1,
2-D vectors, GCD/banerjee independence, constant subscripts, dimension
mismatch).

**S15 CLOSED / PASS** (full S09 **56/0/2**, `test_inline_v0` **8 checks**):
`opt/CostModel.h` defines the immutable `TargetCostModel` service (call
overhead, instruction cost, inline size cap) plus `CostContext` confidence and
a `ConservativeCostModel` used when no target facts exist. `InlineV0Pass`
consumes block frequencies and function summaries: only hot calls (>= 0.5 of
entry frequency) to single-block, non-recursive callees without phi/alloca are
cloned; cost/size caps come from the model. Cold, multi-block, alloca,
recursive and over-budget sites are declined with counters. The pass is not
wired into the default O1 pipeline until a measured benefit exists.
`test_inline_v0` covers hot inline, cold decline, each unsupported class and a
custom model cap (8 checks).

**S14b CLOSED / PASS** (full S09 **56/0/2**, `test_hotness` **19 checks**):
`BranchProbabilityAnalysis` uses loop structure (header biases inside 0.9;
back-edge targets 0.1; otherwise even) and is consumed by
`BlockFrequencyAnalysis` with a bounded relaxation (entry 1e6, cap 1e12,
4*blocks+32 rounds). `ValueFactsAnalysis` computes conservative i32 ranges for
constants/add/sub/mul/sdiv/srem/icmp/select/phi; overflow, unknown operands,
loads and calls stay unknown. `test_hotness` covers loop/latch probabilities,
frequency ordering, exact and unknown ranges and CFG invalidation (19 checks).

**S14 CLOSED / PASS** (full S09 **56/0/2**, `test_cfg_analysis` **20 checks**):
`DominatorAnalysis` wraps the single `ir/DominatorTree` (FlatVerifier's
Cooper-Harvey-Kennedy implementation) at Function scope; `LoopInfoAnalysis`
builds natural loops from dominator back edges with merged tails, sorted block
sets and a parent/depth forest. The AnalysisManager now has a real Loop scope:
`get<LoopViewAnalysis>(function, loopIndex)` caches per loop slot and invalidates
on CFG revisions. `test_cfg_analysis` covers 12 hand CFGs for idom/dominance,
single/nested/sequential/self/multi-backedge loops, an irreducible cycle,
unreachable blocks, Loop-scope caching and CFG invalidation (20 checks).

**S13 CLOSED / PASS** (full S09 **56/0/2**, `test_alias` **37 checks**): `AliasAnalysis`
is a Module-scope consumer of `CallGraphAnalysis` (dependency mask
DataFlow|Memory|Calls|Globals|Interface). It resolves pointer provenance to Alloca,
Global, Param or Fresh bases with constant byte offsets through GEP/BitCast/Phi,
and instantiates callee return summaries at call sites, so a call returning a
parameter resolves to the caller's argument and a call returning a global keeps
that global. Unknown/external results and return offsets that are not provably
exact degrade to MayAlias. `test_alias` covers 37 checks including locals,
globals, arguments, constant/dynamic GEP offsets, cross-function returns,
interior-pointer unknowns and Memory/Calls invalidation.

Post-S12 audit fixes (2026-10-20, full gate **56 PASS / 0 FAIL / 2 expected
SKIP**): memory intrinsics are classified once for both `Call` and specialized
opcode forms; `llvm.memset` is a writes-only effect, `llvm.memcpy` reads and
writes, and unknown/suffixed externals conservatively do both, propagating
through internal and recursive calls; AnalysisManager freshness checks memoize
validated dependencies within one traversal, so a shared DAG is expanded once
per check (32-node DAG from ~5.25 s to <1 ms); FlatReader call arguments track
ID presence separately from the number, so a forward `%0` argument is accepted
while missing/undefined arguments are rejected. Evidence: `test_interprocedural`
**51 checks**, `test_flat_reader` **14 checks**, `test_analysis_manager` DAG
memo check, unit suite PASS.


Post-F05 review fixes (2026-10-19, full gate **56 PASS / 0 FAIL / 2 expected
SKIP**): Mem2Reg's detached-scratch guard now hands incomplete plans to the
caller instead of double-discarding them; FlatReader rejects unregistered value
ids instead of falling back to internal ids, binds legal forward references
through operand fixups, and reports unresolved ids per function; the unit
runner builds the `compiler` target and fails when it is missing; the
initializer checker uses a sparse expected-value table so `sl2.sy` no longer
OOMs under the 8 GB gate limit.

F04 validation (2026-10-19): clean-worktree full S09 gate **55 PASS / 0 FAIL /
2 expected SKIP**; trajectories **357 exact / 62 loop-count-only /
0 observable differences / 121 categorized skips**; Mem2Reg structure
**1 alloca / 1 load / 3 stores → 0 / 0 / 0 + 1 phi**; forged-stat negative
control **PASS**; unit suite **PASS** including `test_mem2reg_pass`
**20 checks / 0 failures**; F04 boundary checker and architecture negative
controls **PASS**. F04 did not rerun the 540-case O1 Flat protocol or target
execution matrices; those remain F01/F03 historical evidence.

F03 `FlatPassManager` runs each named `FunctionPass` exactly once on every
non-declaration Function in **pass-major, Module Function order**, with one
Module-scoped `AnalysisManager` shared by the entire pipeline and fresh
`IRMutator`, `CFGEditor` and `PassContext` for each invocation. Only actual
tracked edits determine `changed` and `MutationSummary`, never a pass's
self-reported change flag. `NoChange`/`Declined`/`Failed` with mutation and
`Applied` without mutation violate the contract and fail closed.
`PreservedAnalyses` is declarative; it cannot override F01/F02 revision/cache
facts. Optional verify-each runs on entry and after each actual invocation;
stable pass-name disable/ablation, optional before/after Flat dumps, timing,
ordered statistics and instrumentation are supported. There is **no implicit
fixpoint**. This does not implement ModulePass, CGSCC/Loop adaptors,
CallGraph, FunctionSummary, AliasAnalysis, LoopInfo, SCEV, target-cost,
CompilationSession or backend integration.

014 validated a **two-part candidate identity** against base
`3f824833cb4d403c926f9f9f0ed2f2dd3a529e0a`: (1) the four original
tracked files (`CMakeLists.txt`, `docs/ARCHITECTURE.md`,
`docs/CURRENT-STATE.md`, `tools/selftest/unit/run.sh`) have binary patch
SHA256 `ea5cc3da5de339310bfbe2800db2c6fa5abda7d654c3a11d7244cb201edf7d51`
and byte-for-byte match 014 `before.diff`; (2) the three then-untracked files
have content SHA256: `src/opt/FlatPassManager.cpp`
`29ac6199cc86de1e1335a7687d2df8cc9c38f91ae9579df7e8e934e9b55b05d6`,
`src/opt/FlatPassManager.h`
`dc8a5c7a1540272fb8d20a421f063ae927c70e0edf292670ceb78ef97bf5652d`,
and `tools/selftest/unit/test_pass_manager.cpp`
`ac60e1abede1a2d26a8fcf8856603a16fe5dc4d446362c19bcce65509fc3fffe`.
014's `before` source fingerprint
`5c23f31c701cbc5435a67a8ed57c2411bdbe576445dbcebe5ae6907dde7c6a4b`
is a **historical validation record**, not a recomputed commit identity.
The seven-file implementation commit legitimately includes three additions,
so its complete patch is **not** compared to the tracked-only `before.diff`.
Post-commit verification instead matched the four-file tracked patch and
all three committed file-content hashes.

014 final acceptance: formal S09 terminal **exit=0, 54 PASS / 0 FAIL /
2 expected SKIP**; S08 **44 invalid / 7 positive / 8 legacy**, S08 and
Mem2Reg gates **PASS**; trajectory **357 exact / 62 loop-count-only /
0 observable differences / 121 categorized skips**. LLVM manifest
**540 total / 490 supported PASS / 50 tensor frontend reject /
0 FAIL / 0 TIMEOUT**. `test_pass_manager` **47 checks /
8 negative controls / 0 failures**; unit runner **exit=0**.
Full O1 Flat protocol: `IDENTITIES=540`, `SUPPORTED=490`,
`BASELINE_TENSOR_REJECT=50`, `CURRENT_TENSOR_REJECT=50`,
`MISMATCH=0`, `ERRORS=0`, `BYTE_IDENTICAL=490`, **exit=0**.
Manifest SHA256
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.
014 observed unchanged HEAD, status and candidate identity before/after
validation, followed by fresh independent web review **PASS / 0 findings**.

Historical task accounting: 011/012 long-validation lifecycles did not obtain
terminal evidence (neither compiler failures nor acceptance); 013 was superseded
by 014 without starting its WSL gate; 015/016 were closeout identity-rule design
errors with **no project changes**, neither compiler failures nor acceptance
evidence. The 017 local closeout does **not** rerun the full S09, 540-case O1
protocol or target execution matrices; independent post-closeout review remains
a separate required handoff.

F02 final acceptance (valid 004 + independent 008): accepted revision
HEAD/clean/fingerprint stayed unchanged during validation and evidence review.
Unit tests **PASS**, `VALUE_USE_FAILURES=0`,
`ANALYSIS_MANAGER_FAILURES=0`. S08 **49 cases / 0 failures**; public matrix
**44 invalid / 7 positive / 8 legacy**, `block_bad_index` **V4** and
ownership negatives **V1**. Full S09 **54 PASS / 0 FAIL /
2 expected SKIP**, `S08_OK=YES`, `MEM2REG_OK=YES`; trajectories
**357 exact / 62 loop-count-only / 0 observable differences /
121 categorized skips**. LLVM manifest **490 supported PASS /
50 tensor expected reject / 0 failure / 0 timeout**. Full O1 Flat:
**490/490 byte-identical**, **0 mismatch**, baseline and current each
**50/50 tensor rejects**. Frozen pre-F02 baseline compiler SHA256:
`15235a6fa38026df9b06dd21750f19e029b4c263dd97955a59c6754333cd7d06`;
manifest SHA256:
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.
Ownership regressions fail closed. Architecture, mutation-boundary, line-budget,
CLI, syntax and git-diff gates **PASS**. Independent web review 008 actually
checked the original evidence and reported **PASS / 0 findings**.

**REUSED FROM 59885f9 VALIDATION** (not rerun at `7fdb9f3`): full
x86 O1 **490 PASS / 50 SKIP / 0 FAIL / 0 TIMEOUT**; full
no-structured O1 **490/50/0/0**; focused O0 normal **3/3**, O0
no-structured **3/3**, AArch64 O1 **3/3**, RV64 O1 **3/3** and
toy O1 **3/3**. The exact Fix3 diff from `59885f9` changes only the
FlatVerifier V1→V4 classification and its corresponding unit test; none of
these target matrices was claimed as rerun on Fix3. Attempts 001/002/003 were
**INVALID_TEST_STATE** due to validation environment/working-directory setup,
not compiler failures and not stage acceptance evidence.
Order: F00 → F01 → F02 → F03 → F04 → F05 → S12+. B00 read-only target/ABI
audit and B01 MIR design may interleave only within their documented boundaries;
formal Flat→backend integration must not bypass the foundation. S10b remains
separately dependent on S10a, S11, and S11b evidence.

## Validation ledger

Latest S07 LLVM manifest run (2026-09-29):
`python3 tools/selftest/check_llvm_ir.py --root /home/koki1/try --compiler
build/compiler --llvm-as /usr/lib/llvm-18/bin/llvm-as --jobs 8 --timeout 30
--work /tmp/s07-final4 --report /tmp/s07-final4.json` reports **490 PASS, 50
FRONTEND_REJECT, 0 FAIL, 0 TIMEOUT**. Every supported `.ll` assembled with LLVM
18.1.3; each tensor item produced a diagnostic and no `.ll`. Archived phase report
and per-case JSON evidence are in the companion workspace at
[`S07 report`](../../docs/handoff/reports/07-S07.md) and
[`manifest JSON`](../../docs/handoff/reports/07-S07-llvm-manifest.json).

The S07 emitter fixes include LLVM `\xx` quoted-name escaping, `fpext` values
typed as `double` at their variadic `putf` ABI boundary, and specific tensor
frontend-rejection diagnostics recorded per manifest case. The manifest checker
has negative controls for unrelated errors being misclassified as tensor
rejections. The 2026-09-29 rerun reports **490 PASS, 50 FRONTEND_REJECT, 0 FAIL,
0 TIMEOUT**. S05b reports 399 trajectory matches, 0 differences, and 141
explicit skips: 89 step-budget, 50 tensor frontend rejection, and 2 recursion
depth. Skips are not passes; the gate passed its minimum 200-case coverage
because 399 cases were actually compared.

The earlier integrated S07 run failed its S05b launch while `build/compiler`
was concurrently rebuilt, and its S11 target checks also failed. Both records
are retained. A fresh complete S06 gate on the committed S07 source state then
passed before S08 edits:

```bash
ulimit -v 8388608 && timeout 2400 bash compiler/tools/verify.sh --stage S06
```

It reported 48 passed, 0 failed, and the expected S11 execution skip. The flat
versus structured comparison reported 359 identical full trajectories, 60
loop-count-only differences, 0 observable differences, and 121 categorized
skips. The complete run also passed the 490-supported / 50-tensor LLVM
manifest gate. This is the pre-S08 baseline, not a claim of S11 target-runtime
correctness. The earlier incomplete-run record remains in the dated S07 report;
see also the [fresh S06 baseline](../../docs/audit/S06-BASELINE-S07-2026-09-30.md).
LLVM assembly proves text acceptance only; it does not prove target execution
correctness or performance.

The post-fix S08 entry gate was rerun on a clean source revision after the
FlatVerifier closeout review:

```bash
ulimit -v 8388608 && timeout 2400 bash compiler/tools/verify.sh --stage S08
```

It reported **49 passed, 0 failed, 3 expected stage skips** (S09, S11 target
execution, and S10a interpreter evidence are not required by this stage). The
490-supported /
50-tensor LLVM manifest gate passed; flat versus structured execution reported
357 identical trajectories, 62 loop-count-only differences, 0 observable
differences, and 121 categorized skips. The S08 public-boundary matrix passed
44 invalid, 7 positive, and 8 mapped legacy cases. This supersedes the earlier
49/0/2 and 28/7/8 historical baseline for the current source. The gate also
passed the dynamic remainder regressions and independent IRGen comparison.

The historical pre-F01 post-fix S09 gate passed on the same clean source revision:

```bash
ulimit -v 8388608 && timeout 2400 bash compiler/tools/verify.sh --stage S09
```

That pre-F01 run reported **52 passed, 0 failed, 2 expected stage skips**. The promoted flat
path ran across 540 files: **357 identical complete trajectories, 62
loop-count-only differences, 0 observable differences, and 121 categorized
skips**. S08's public matrix again reported 28 invalid / 7 positive / 8 legacy
cases in the historical baseline and 44 / 7 / 8 in this post-fix run; the same
gate passed the 490-supported / 50-tensor LLVM manifest. Its designated fixture
changed from 1 alloca, 1 load, and 3 stores to no memory instructions and 1 phi.
A no-op CLI that forged the promotion statistic was rejected. The fresh run
proves interpreter-observed behavior preservation and a real structural
transformation, not target runtime correctness or performance.

The post-fix S10a isolated runner separates recipe generation from
`s10a-recipe-oracle-v1`, checks a generated-expectation negative control, and
atomically writes a RUNNING/FAIL/PASS JSON manifest with source revision and
artifact hashes. PASS requires a clean compiler worktree and all required log
markers. The fresh invocation was:

```bash
bash tools/selftest/interpreter/run_s10a.sh --keep \
  --report ../docs/handoff/reports/S10A-REVIEW-FIX-2026-09-30.json
```

It reported **115/115 fixed cases on both runs and 32/32 generated cases** with
seed `24176631846`; replay, the independent oracle, both negative controls,
constraints, hash vectors, and every resource-limit check passed. The
[revision-bound manifest](../../docs/handoff/reports/S10A-REVIEW-FIX-2026-09-30.json)
records the exact clean revision and artifact hashes. The interpreter defines
`getch` bytes as unsigned; target comparisons are limited to ASCII until target
behavior is measured. Target differential remains an S10b/S11/S11b task.

S05 defines integer `% 0` as the dividend based on cross-target measurements.
The interpreter, flat execution helper, and constant evaluator already follow
that rule. The current IRGen fix makes conditional-result branches return their
values explicitly, so dynamic `x % 0` returns `x` instead of an inferred zero;
focused positive, negative, ordinary-remainder, and `INT_MIN % -1` regressions
and both post-fix stage gates pass. A target-backed regression is still required
before claiming target agreement.

Capability requirements: S07 requires real `.ll` validation with `llvm-as` and
the exact supported manifest. S11 requires the three target pipelines and exact
manifest execution evidence; S11b requires the same contract through the toy
backend. `--quick` is a smoke check and cannot satisfy a requested capability
gate. The 490 non-tensor / 50 tensor split is a corpus scope fact, not a result
invented by this document. Full corpus gates may require the workspace's
external corpus and runtime; standalone clones must remain portable without
those assets for local checks.

**S24a CLOSED / PASS** (full S09 **57/0/2**, `test_tile` **20 checks**):
`planTiling` derives a tile from cache capacity and element size: the largest
power of two whose two-stream working set fits half the cache, bounded by the
iteration count and a minimum of 8. It declines a full working set that already
fits, cold nests, too-small caches and empty shapes; an optional bounded probe
can accept/reject the derived candidate. Tests cover 2-D matmul, 3-D stencil,
64/32/16 KiB cache variants, exact-capacity boundaries, one byte below a
boundary, overflow saturation, rejection paths and both probe outcomes. Integer
capacity comparisons replace floating-point roots, so an exact-fit tile selects
the largest fitting power of two (including the 7-D, 32 MiB-cache regression);
a unit extent does not iterate over the dimension count.

**S24b PARTIAL — order-preserving strip-mining**: `LoopTilingPass` rewrites
a canonical constant-trip loop into a tile header, the original loop body and a
tile latch. The tile size comes from `planTiling` (cache/element size, hotness);
the loop order is unchanged because the tile groups consecutive iterations, so
the transform is safe for ordinary bodies (stores/calls included) without a
dependence proof. `CFGEditor::replaceTerminator` gained a phi-values overload
for newly added edges and rejects duplicate incoming values on retained edges.
The matcher now rejects loop-defined live-outs and non-canonical exit phis before
editing; non-unit steps advance by `T*step` and clamp to the terminal value on
the original recurrence. Tests: `test_cfg_phi_edges` **12** and
`test_tiling_transform` **47**; CTest `test_tiling_equivalence` executes seven
actually-transformed before/after modules and compares FlatExec observables,
including the step-3 oracle (6048).

Still missing for S24b: two-dimensional blocking (which reorders tiles and
needs a dependence-legality proof), matmul/conv/stencil fixed-protocol runtime
measurements, and target-backed evidence. No runtime gain is claimed.

**S24c-a CLOSED / PASS (planning)** — `BlockingPlan` selects a 2D tile
(`tileOuter`, `tileInner`), records the tile footprint, chooses row-major or
column-major blocked order from unit strides, and declines dependence, cold,
small, fitted and over-capacity cases. Footprints are cache-line upper bounds,
not raw element bytes; `dependenceFree` defaults to false so callers must opt in. `test_blocking` has **22 checks**
(exact half-cache line footprint, one element above/below, large-trip sizing,
column-major, dependence, cold, SIZE_MAX saturation, oversized element, and
zero/one-byte cache-line saturation). This planner does not rewrite IR;
`LoopBlockingPass` and `MatmulBlockingPass` consume it.

**S24c-b PARTIAL — blocked reduction and map**: `LoopBlockingPass` matches a
canonical two-level nest with either:
- an integer-add reduction `g = g + f(i,j)` (one scalar-global load/store), or
- a read-only-input/write-only-output map whose store indices are exactly the
  two loop indices and whose one or more global loads all come from bases
  distinct from the output (e.g. a two-input/neighbor stencil
  `out[i][j] = f(in[i][j], in[i][j+1], in2[i][j])`).

Both are pure (no calls/alloca/memcpy), have no live-outs, and require both tile
dimensions to be strictly partial. The reduction is legal because i32 addition is
associative/commutative; the map is legal because the input is never written and
the output address is unique per `(i,j)`. The pass consumes `BlockingPlan`,
builds i0/j0/i/j, clones the body, and neutralizes the now-unreachable original
header phis. `test_blocking_transform` has **60 checks**; CTest
`test_blocking_equivalence` executes exact/partial reductions, scalar/array
reduction, a 128x128 map, a nested-preheader reduction and shifted/flat-map negatives, requiring identical final memory/return (write order differs).
This is not matmul/conv accumulation and has no target runtime; the transforms
are available only through opt-in pipelines (`--opt=s24` for tiling, `--opt=s24b` for blocking,
`--opt=s25` for unrolling), never by default. Both and their combination pass the
540-file corpus with zero observable differences (349 exact + 70 loop-count +
121 skip); 50 loops in 38 files actually tiled and **12 maps blocked**. S24b remains order-preserving strip-mining. Empty inner-preheader trampolines
are transparent, so those maps block; broader nested canonicalization and
general convolution accumulation remain open; canonical matmul is described below.

**S24c-c PARTIAL — canonical matmul accumulation**: opt-in `--opt=s24c`
performs canonicalization, DCE and `MatmulBlockingPass`. `MatmulMatch.cpp`
proves an O/J/K nest with zero-based unit-step constant trips, a local i32
accumulator in K, read-only global inputs and one unique canonical C[i][j]
store. Every header must branch on its matched comparison. Exact predecessor
sets, bounded empty trampoline paths, clean induction latches, an unconditional
outer preheader, no live-outs and a phi-free exit are checked before editing.
The retained K region cannot depend on definitions in discarded blocks.

The transform builds tile-I/tile-J/I/J loops and preserves each output's K
iteration order. The new J edge goes directly to K with explicit zero seed
values for both phis, including when the original nest has no K preheader.
Live operand changes use the tracked mutator. Unsupported shapes decline with
identical IR; unexpected editor failures abort the pipeline, without a general
structural rollback claim.

The review of `5e0fd6b` reproduced wrong-code from an unrelated header condition
and invalid SSA from a conditional outer preheader. Both are now guarded;
exit-phi rejection and direct J-to-K insertion are also covered.
`test_matmul_match` has **78 checks**. `test_matmul_equivalence` executes three
actual rewrites (64x64x64, 65x67x7, 67x65x5), checks every C element against an
independent arithmetic oracle, compares memory/return/input state and requires
reordered stores. `test_stencil_equivalence` exercises a 64x64 1-D 9-tap stencil;
`test_conv_equivalence` exercises a flattened 2-D 3x3 convolution with an
offset table, both with full output oracles. A canonical nested-K 2-D conv
(ki/kj accumulator chain) is also blocked for 3x3 and 5x7 kernels and checked
with 4096-cell oracles;
this is not a general multi-dimensional convolution claim. `matmul_bench.py` provides a fixed native-host Clang -O0
pipeline comparison with full-C correctness, warmups/repeats, median/spread,
compile time/RSS and output sizes. It includes canonicalization/DCE effects;
its timings do not isolate the blocking pass or establish A53/BOOM performance.
The ordinary corpus has no known matmul firing; fixtures carry this evidence.
Convolution, target-calibrated tile costs, own-backend runtime and real target
hardware remain open. Current full-gate status is recorded in the versioned
review report; the interrupted `5e0fd6b` run is not acceptance evidence.

**S25a CLOSED / PASS** (full S09 **57/0/2**, `test_unroll` **23** +
`test_unroll_advisor` **13** checks): `planUnroll` now consumes a critical-path
estimate (capping UF at body/criticalPath), enumerates power-of-two VF
candidates jointly with UF for AArch64 (product at most eight) and scalar
unroll for RV64, applies the `registers/(live+1)` cap, the nonlinear pressure
penalty with its 0.8 cliff, and a bounded probe. `adviseUnrolls` consumes
BlockFrequencyAnalysis + LoopInfoAnalysis for canonical constant-trip loops,
recording trip count, body size, hotness and the resulting plan; non-constant
trips are skipped rather than guessed. The register guard rejects
`live >= registers` before `live+1` and SIZE_MAX boundaries are covered.

Limits, recorded rather than hidden: there is no per-register-bank machine
model, no spill estimate and no backend calibration; those belong to S25b
together with the actual unroll/vector transform and runtime protocols.

**S25b PARTIAL — plan-sized chain unrolling**: `LoopUnrollPass` matches a
two-block canonical loop (one i32 phi, constant start/bound/positive step, one
body, no loop-defined live-outs, no body phi/alloca). The chain length is
`min(plan.factor, 4)`; the header guard becomes
`i < bound - (factor-1)*step`, the chain executes `factor` body copies at
`i..i+(factor-1)*step`, and the header advances by `factor*step`. A single tail
loop consumes the remainder one body at a time, so every representable, non-overflowing trip count >= 2 is
covered; overflowing recurrences decline unchanged. `test_unroll_transform` has **112 checks**; CTest
`test_unroll_equivalence` executes ten genuinely transformed cases
(even/odd/step-3, non-aligned, near i32 bounds, wide step) and compares FlatExec
observables. Body operands are prevalidated; arguments are cloneable. Vectorization, pressure/spill measurements and per-target runtime
remain S25b follow-up; the pass is not in the default pipeline.

## S23 audit fixes (2026-10-20)

An independent review of the S12–S23 delivery found seven issues; all are
fixed with regressions:

- DSE now removes only overwritten stores to non-escaping stack slots (the
  global write trace is observable in the oracle); the old reverse scan erased
  the later store, changing `g=1; g=2; return g` from 2 to 1.
- `InlineV0Pass` snapshots block frequencies and function summaries before its
  first mutation, removing a stale-handle crash on two consecutive inlines.
- `LoopRotatePass` requires the header to contain only the condition and
  clones only that condition; header side effects can no longer be bypassed.
- `LoopInterchangePass` validates one i32 induction phi per header, constant
  start/bound/positive step, predecessor sets, in-region uses and a single
  pure/load body before editing; unsupported shapes are declined without a
  crash, and post-mutation edit failure fails the pipeline.
- `AliasAnalysis` depends on `MutationKind::DataFlow`; changing a returned
  pointer invalidates and recomputes the alias result.
- The unused SCEV field was removed; Clang builds warning-free.

Evidence: `test_alias` 37, `test_s19` 7, `test_inline_v0` 14,
`test_looprotate` 7, `test_interchange_transform` 12 checks; Clang CTest 37/37;
DSE regression behavior returns 2 with an unchanged global write trace.
Full dated report: [`../docs/audit/S23-AUDIT-FIXES-2026-10-20.md`](../../docs/audit/S23-AUDIT-FIXES-2026-10-20.md).

## Reading paths

Read `src/frontend/` for source semantics and runtime adaptation,
`src/structured/` for structured ownership and lowering, `src/ir/` for the
flat data model and verifier, and `src/support/` for diagnostics and shared
utilities. Read the matching contract in `docs/ARCHITECTURE.md` before
crossing one of those boundaries.
