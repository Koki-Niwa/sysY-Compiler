# Performance extensions

The optimization goal is generated-assembly runtime performance after
correctness gates pass. Keep a baseline from S11 onward; do not defer all
measurement until S26. No result belongs in this document until it has been
measured.

Research references for the representation and pass boundaries are the
[MLIR rationale](https://mlir.llvm.org/docs/Rationale/Rationale/),
[MLIR pass management](https://mlir.llvm.org/docs/PassManagement/),
[Polly architecture](https://polly.llvm.org/docs/Architecture.html), and egg's
[Runner](https://egraphs-good.github.io/egg/egg/struct.Runner.html) and
[Extractor](https://egraphs-good.github.io/egg/egg/struct.Extractor.html)
documentation. They are conceptual references only; do not copy external
implementation or add a dependency on those projects.

## Multi-level IR policy

Retain loop structure, access expressions, effects, and source locations while
they inform a transformation. Introduce a new IR only when a concrete
transformation needs a representation that the current level cannot express.
Every lowering must preserve the required information, run a verifier, and
have a lowering test. There is no fixed maximum IR count. A level that has no
consumer is a removal candidate.

## Affine and polyhedral path

The affine path must derive, then verify, an iteration domain, affine memory
accesses, dependence relations, legal schedules, and code generation. It may
fall back to the original loop when any proof is missing. It must account for
i32 wraparound, proven bounds, aliasing/effects, and floating-point order;
syntax that merely looks affine is not enough. The staged path is:

```text
normalize -> detect/domain -> access/dependence -> legal schedule -> lower/codegen
                                                     \-> conservative fallback
```

## E-graph path

Typed e-graphs are a future option for pure regions with legal rewrite rules.
Start with explicit types, wraparound and floating-point guards, and a
bounded runner (iterations, nodes, and wall time). Retain the original
candidate and a safe fallback. Separate legality from target-aware cost
extraction. Existing DSL rules may be migrated only after their semantic
preconditions are represented; do not generate rules from syntax without
proving them.

## Whole-program analysis

Build call SCCs. Acyclic components can be processed bottom-up. Self-recursive
components need a bounded fixed point or a conservative summary; mutual
recursion must not be assumed absent merely because current corpus scans found
none. Closed-world visibility, effect summaries, alias summaries, and
invalidation are explicit inputs to inlining and interprocedural transforms.

## Target boundary

Current target-independent LLVM IR omits triple and layout. Backend owns actual
target configuration. A future target cost model may guide profitability, but
target selection and host defaults must not leak into frontend semantics.

## Measurement record

For each comparison record target and toolchain, input identity/hash, warmup
and repeat count, median and spread, correctness verdict, per-case regressions,
aggregate speedup, assembly/binary size, compile time, peak RSS, pass
ablation, and separate SKIP/TIMEOUT counts. Fix backend toolchain and
optimization parameters, and use pass on/off ablation to separate this
compiler's benefit from external backend optimization. QEMU may validate
cross-target correctness; QEMU time is a proxy and cannot support a real
ARM/RISC-V hardware performance claim. If target hardware is unavailable,
label the metric proxy or unmeasured. A smoke check is never a full acceptance
result.
