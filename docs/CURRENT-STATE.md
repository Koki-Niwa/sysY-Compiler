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
target-independent and does not include triple or data layout. `-O1` remains
an option record and does not enable optimization.

## Next milestone

S07, the S08 verifier boundary, S09 mem2reg, and the S10a direct AST interpreter
are implemented. A later closeout review found gaps in the dynamic remainder
lowering, FlatVerifier structural/type checks, S09's proof that the CLI really
performed promotion, S10a's generated-case oracle and durable evidence, and the
stage dependency model. The working tree contains fixes for those findings, but
no build, self-test, or stage gate has been run after the fixes. The archived
S08 **49/0/2**, S09 **51/0/2**, and S10a **115/115 twice plus 32/32** results are
historical baselines for the earlier committed source, not acceptance evidence
for the current working tree.

The next acceptance action is a fresh focused and full rerun with a clean source
revision, including an archived S10a evidence manifest. S11 target execution and
its performance baseline follow that rerun. S10b target differential work waits
for S11 and S11b.
End-to-end execution and target-code runtime measurements remain unimplemented
and must not be inferred from LLVM assembly validation.

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

The historical S08 entry gate was rerun after the original FlatVerifier audit
and its independent review:

```bash
ulimit -v 8388608 && timeout 2400 bash compiler/tools/verify.sh --stage S08
```

It reported **49 passed, 0 failed, 2 expected skips** (S11 target execution and
S10a interpreter evidence are not required by this stage). The 490-supported /
50-tensor LLVM manifest gate passed; flat versus structured execution reported
359 identical trajectories, 60 loop-count-only differences, 0 observable
differences, and 121 categorized skips. The S08 public-boundary matrix passed
28 invalid, 7 positive, and 8 mapped legacy cases. See the [fresh S08 gate
report](../../docs/audit/S08-FRESH-2026-09-30.md) for the complete evidence and
the initial compliance-check failure followed by its fix and passing rerun. A
later review found missing predecessor-cache, malformed ownership/index, phi
cardinality, operand/type, and unreachable-dominance cases; the current fixes
have not yet been rerun.

The historical S09 gate passed after the candidate transaction, phi predecessor
ordering, and negative-test fixes:

```bash
ulimit -v 8388608 && timeout 2400 bash compiler/tools/verify.sh --stage S09
```

It reported **51 passed, 0 failed, 2 expected stage skips**. The promoted flat
path ran across 540 files: **359 identical complete trajectories, 60
loop-count-only differences, 0 observable differences, and 121 categorized
skips**. S08's public matrix again reported 28 invalid / 7 positive / 8 legacy
cases; the same run passed the 490-supported / 50-tensor LLVM manifest. The
fresh run proves interpreter-observed behavior preservation, not target runtime
correctness or performance. See the [S09 fresh gate report](../../docs/audit/S09-FRESH-2026-09-30.md).
That gate did not prove that `--mem2reg` changed the CLI-produced flat IR: the
current gate adds a designated memory-form fixture, structural before/after
checks, promotion statistics, and a no-op CLI that forges statistics as a
negative control. These additions have not yet been run.

The historical S10a isolated runner invocation
`bash tools/selftest/interpreter/run_s10a.sh --keep` reported 115/115 fixed
cases on both runs and 32/32 seeded cases (seed `24176631846`). Its generated
expected values were computed in the generator's own mode branches, and its
only retained artifacts were under `/tmp/sysy_s10a_KtKIA7`; therefore that run
does not establish an independent, revision-bound generated-case oracle. The
current runner separates recipe generation from `s10a-recipe-oracle-v1`, adds a
generated-expectation negative control, and can atomically write a
RUNNING/FAIL/PASS JSON manifest with source revision and artifact hashes. PASS
requires a clean compiler worktree and all required log markers. After the
current changes are committed, archive a fresh manifest with, for example:

```bash
bash tools/selftest/interpreter/run_s10a.sh --keep \
  --report ../docs/handoff/reports/S10A-FRESH-YYYY-MM-DD.json
```

No such post-fix PASS manifest exists yet. The interpreter defines `getch`
bytes as unsigned; target comparisons are limited to ASCII until target
behavior is measured. Target differential remains an S10b/S11/S11b task.

S05 defines integer `% 0` as the dividend based on cross-target measurements.
The interpreter, flat execution helper, and constant evaluator already follow
that rule. The current IRGen fix makes conditional-result branches return their
values explicitly, so dynamic `x % 0` returns `x` instead of an inferred zero;
focused positive, negative, ordinary-remainder, and `INT_MIN % -1` regressions
were added but have not been run. A target-backed regression is still required
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
