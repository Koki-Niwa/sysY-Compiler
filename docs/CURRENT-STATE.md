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

**S16 CLOSED / PASS** (full S09 **56/0/2**, `test_dependence` **13 checks**):
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

**S13 CLOSED / PASS** (full S09 **56/0/2**, `test_alias` **33 checks**): `AliasAnalysis`
is a Module-scope consumer of `CallGraphAnalysis` (dependency mask
Memory|Calls|Globals|Interface). It resolves pointer provenance to Alloca,
Global, Param or Fresh bases with constant byte offsets through GEP/BitCast/Phi,
and instantiates callee return summaries at call sites, so a call returning a
parameter resolves to the caller's argument and a call returning a global keeps
that global. Unknown/external results and return offsets that are not provably
exact degrade to MayAlias. `test_alias` covers 33 checks including locals,
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
0 FAIL / 0 TIMEOUT**. `test_pass_manager` **41 checks /
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

## Reading paths

Read `src/frontend/` for source semantics and runtime adaptation,
`src/structured/` for structured ownership and lowering, `src/ir/` for the
flat data model and verifier, and `src/support/` for diagnostics and shared
utilities. Read the matching contract in `docs/ARCHITECTURE.md` before
crossing one of those boundaries.
