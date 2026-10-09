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

The active next milestone is **S11b toy/self-built lowering contract**. It must
actually replace LLVM `llc` as the lowering provider for target-neutral `.ll`
and validate x86 correctness execution; accepting a `--toy-backend` flag alone
does not meet the contract. This is protocol-consumer validation, not the
formal performance backend. After S11b, F00–F05 remain mandatory before S12.
F00–F05 → S12+ ordering remains in force; B00 read-only design and B01 MIR
design may interleave as planned, but live Flat→formal backend integration must
not bypass the foundation. S10b target differential waits for S11/S11b evidence.

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

The post-fix S09 gate passed on the same clean source revision:

```bash
ulimit -v 8388608 && timeout 2400 bash compiler/tools/verify.sh --stage S09
```

It reported **52 passed, 0 failed, 2 expected stage skips**. The promoted flat
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
