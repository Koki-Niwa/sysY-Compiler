# Compiler goals

## Primary outcome

Produce a correct, portable SysY2022 compiler whose self-built backend emits
GAS assembly for the competition targets. Correctness and established language,
ABI, IR, and competition constraints are hard gates. After those gates, the
product objective is the highest practical generated-assembly runtime
performance on ARMv8-A/Cortex-A53 and RV64GC/BOOM v3. Target-independent LLVM
IR remains an oracle/debug/stable protocol boundary; it is not the final product
boundary.

Choose optimization work by measured runtime benefit on representative inputs.
Record compile time, peak RSS, emitted IR/assembly size, and binary size as
costs. A performance claim needs measurements, a stated scope, and a per-case
regression policy; an aggregate mean must not hide a useful case that regressed.
Infrastructure and representation changes may instead demonstrate a concrete
extension need or reduced coupling, with correctness and resource checks.
Record that enabling benefit separately from measured runtime gains.

## Architecture principles

Correctness remains a hard constraint. For architecture choices, apply Codex
readability first and functional modularity second: each module exposes a
small contract, owns its state, and is understandable without reading the
whole repository. This does not justify deleting information needed by a
profitable optimization or weakening independent validation.

## Representation strategy

Keep multiple IR levels when each level preserves information needed by a
real transformation or makes a correctness boundary explicit. Lowering is
one-way at a reviewed boundary, with a verifier and a lowering test. There is
no fixed maximum number of levels and no requirement to introduce a level
without a concrete consumer.

The structured IR retains loops and access structure; the flat IR provides CFG
and SSA for general lowering and emission. Future affine, e-graph, and
whole-program analyses may consume the structured or flat form as their
contracts require.

## Scope and evidence

Current work is SysY2022 without tensor syntax. Tensor cases remain an explicit
known scope boundary until separately specified and implemented. Historical
counts in audit reports are evidence about those runs, not current validation.
Every new performance claim must name the target, toolchain, input set, and
measurement method.
