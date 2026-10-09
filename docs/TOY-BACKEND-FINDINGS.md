# S11b Toy Backend Contract Findings

**Scope:** independent C++17 consumer of current LLVMTextEmitter text and
x86-64 SysV GAS lowering; this is neither a formal backend nor proof of
490-case coverage. Evidence: executable handcrafted IR fixtures, a real 14-case
manifest smoke (functional, h_functional, performance), and 3-case focused
normal S11, no-structured S11 and unavailable-LLVM toy regression.

An earlier, separate 11-case toy smoke at only 30 seconds per case yielded
10 PASS and one 30-second timeout (final_arm/performance/h-1-01). That case
has not been retested at the S11b 600-second correctness timeout. **Full 490
required / 50 tensor excluded evidence: NOT RUN.**

Authority: actual LLVMTextEmitter > current BACKEND-HANDOFF > current
runner/gate > historical S11b prompt. Product src/** was not changed.
CORPUS_OBSERVED means only the short focused cases; FIXTURE_CONFIRMED
means executable handwritten IR. Neither is full-corpus CONFIRMED.

## The twelve contract points

### 1. Instruction-set closure

- Contract claim: only current emitter's closed textual subset, including
  select, zext/trunc, shifts, bitwise operators, call and two memory intrinsics.
- Actual observation: emitter switch emits these forms; focused manifest
  parsed and executed. Parser rejects unknown opcodes instead of guessing.
- Toy consumer requirement: no general LLVM fallback, no silently ignored syntax.
- **Status: CORPUS_OBSERVED** (not all 490).

### 2. Entry-block alloca

- Contract claim: every alloca resides in the entry basic block.
- Actual observation: emitter formats alloca with alignment; fixtures and
  selected real tests ran. Parser rejects an alloca in any non-entry block.
- Toy requirement: fixed entry-time frame allocation, no dynamic RSP per inst.
- **Status: CORPUS_OBSERVED** (partial).

### 3. Phi placement and contiguity

- Contract claim: all phi instructions form a contiguous block prefix.
- Observation: executable loop fixture with three phis; malformed late phi
  rejected. No exhaustive real optimized-phi census was performed.
- Toy requirement: parse only the prefix; fail closed for later phi.
- **Status: FIXTURE_CONFIRMED**.

### 4. Phi predecessor completeness

- Contract claim: one incoming value per CFG predecessor.
- Observation: validation compares predecessor sets and detects duplicate or
  missing inputs. A phi-cycle test executes correctly.
- Toy requirement: all incoming sources first copied to edge temporaries,
  then to destination SSA slots, including conditional edges.
- **Status: FIXTURE_CONFIRMED**.

### 5. GEP / index / address width

- Contract claim: 64-bit pointer, one i64 index, byte scale determined by
  sizeof(source_element_type).
- Observation: actual GEP emitter writes an i64 index; Flat pointer-to-pointer
  BitCast is textually rewritten as getelementptr i8, ptr ..., i64 0.
  A literal zero index is allowed; it need not be defined by sext.
  Array and byte-offset GEP executable fixtures passed.
- Toy requirement: typed scale, not a hardcoded 4; reject inbounds, i32
  indices and additional indices outside current subset.
- **Status: CORPUS_OBSERVED** for focused GEP behavior.
  **CONTRACT_DOC_STALE** for historical requirement that all indices
  originate in sext.

### 6. Inbounds absence

- Contract claim: no inbounds GEP.
- Observation: emitter does not append inbounds; negative parser test rejects it.
- Toy requirement: exact shape, no speculative flags.
- **Status: CORPUS_OBSERVED** (selected cases only).

### 7. nsw/nuw/fast absence

- Contract claim: no these arithmetic modifiers.
- Observation: emitter writes bare opcodes. Negative nsw and fast fixtures
  reject unsupported syntax.
- Toy requirement: signed/unsigned 32-bit arithmetic wraps; no implicit flags.
- **Status: CORPUS_OBSERVED** (selected cases only).

### 8. Zero-global representation

- Contract claim: zero globals are initialized explicitly and lowered to
  sparse BSS, while nonzero aggregates preserve element layout.
- Observation: appendGlobal emits zeroinitializer for empty data and nested
  typed array initializers otherwise. A 1.65-GB global fixture produces
  one .zero 1650000000 under .bss and a tiny assembly file.
- Toy requirement: no GB-sized buffers or expansion; .long words, sparse gaps,
  emitted alignment.
- **Status: FIXTURE_CONFIRMED**, actual emitter text inspected.

### 9. Array parameters as ptr

- Contract claim: opaque ptr parameters, not typed pointer-to-pointer syntax.
- Observation: emitter's llvmType prints ptr; parser accepts ptr params,
  disallows aggregate-valued params.
- Toy requirement: 64-bit SysV pointer ABI, no SysY AST shape metadata.
- **Status: FIXTURE_CONFIRMED** (no exhaustive manifest param census).

### 10. FCmp predicates

- Contract claim: only oeq, une, olt, ole, ogt, oge.
- Observation: toy parses only these predicates; executable tests check
  NaN unordered une versus ordered oeq and +0/-0 equality.
- Toy requirement: parity handling after ucomiss; not plain != for une.
- **Status: FIXTURE_CONFIRMED**.

### 11. i64 use boundary

- Historical contract claim: i64 appears in just four contexts.
- Observation: current emitter has general i64 type printing and can emit
  generic i64 arithmetic and comparisons; current handoff explicitly notes
  64-bit intermediate accumulation. A specific def-shape rule would be stale.
- Toy requirement: validate operand width and supported opcode combinations,
  including i64 math, shifts, sext/zext/trunc, GEP and intrinsic length.
- **Status: CONTRACT_DOC_STALE**. Do not edit product emitter for this.

### 12. Main return type

- Contract claim: defined main returns i32.
- Observation: parser enforces this; short manifest smoke observed it.
- Toy requirement: return via eax per x86 SysV.
- **Status: CORPUS_OBSERVED** (selected cases only).

## Coverage, limitations, and anomalies

- Variadic ABI: declaration of putf(ptr, ...) parses. Handwritten executable
  fixture checks fpext float -> double and SysV variadic %al vector count
  using a C variadic callee. **Actual manifest putf: UNEXERCISED.**
- Integer overflow edge: executable tests confirm INT_MIN/-1 division yields
  INT_MIN, and remainder yields 0 without x86 #DE. Divide-by-zero behavior is
  not invented or repaired by the consumer.
- Intrinsics: both memcpy and memset execute via libc; exact i8/i64/i1
  argument shapes are checked. Runtime uses the existing clang link policy.
- Fail-closed tests: unknown opcode, malformed type, incorrect GEP width,
  non-entry alloca, bad phi, duplicate SSA name, unknown local, missing
  terminator and target-specific directives are errors with line/source/reason.
- Provider evidence: exact toy lowering invocation, realpath and SHA256,
  version, lowering mode and negative checker controls guard against fake
  toy flags or actual llc lowering.
- **VIOLATION**: none conclusively established by focused evidence.
  This does not establish complete protocol closure.
- **UNEXERCISED**: full 490/50 S11b gate, 600-second threshold for the
  timed-out performance case, exhaustive dynamic opcode/predicate coverage,
  actual-corpus putf, all external globals, and accurate peak RSS measurement.
  These require independent long validation and must not be reported as PASS.
