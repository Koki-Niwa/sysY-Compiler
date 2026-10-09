# S11b Toy Backend Contract Findings

**Status: S11b closed.** Accepted compiler revision:
`7648f928fcc4cd45dad66a2b97ce5bfe22875a01`.
The independent long validation completed the full manifest scope: **490
required PASS / 50 tensor SKIP / 0 FAIL / 0 TIMEOUT**, with `format_diff = 127`.
All format-difference cases passed the frozen C1–C3 output-matching contract.
The per-case timeout was 600 seconds for correctness hang detection, not a
performance threshold. This document records the evidence and its limits; the
toy backend remains a correctness-only contract consumer, not a formal backend.

The earlier 11-case smoke at a 30-second timeout had one timeout. That history
is superseded for S11b acceptance by the independent 600-second full run.
S11b did not modify `src/**`; the compiler binary remains identical to the
frozen S11 binary. Full-corpus evidence is now available for the supported
manifest cases, with the observations below scoped to those emitted `.ll`
files. Handwritten executable fixtures remain the evidence for behavior not
exercised by the corpus.

The self-built provider was `/home/koki1/try/compiler/build/toy_backend`,
SHA256 `ca1abc57312fcb352f7968d1841297b466664c5df4829fe69d84eea12e5b3063`,
version `self-built-toy-x86 1`. Its dynamic dependencies contain **NO LLVM
library dependency**. The provider audit found 490/490 toy lower invocations,
0 llc lowering, 0 clang/gcc lowering, and 0 compiler invocations containing
`--toy-backend`. A focused no-LLVM smoke passed; fake-llc, wrong-path, and
wrong-hash reports were all rejected by the formal checker. Runner clang is
still used for assembling/linking generated `.s` with `runtime/sylib.c`; that
is not lowering-provider use.

The manifest SHA256 was
`143fdf139c21782575ab1899f312d82ca4b6ceace4a39329636f54035b443a35`.
The corpus evidence is x86 source-boundary execution through the self-built
toy provider. It is correctness evidence only: it does not establish real
target hardware performance or accurate peak RSS.

Authority: actual LLVMTextEmitter > current BACKEND-HANDOFF > current
runner/gate > historical S11b prompt. Product `src/**` was not changed.
`FULL_CORPUS_CONFIRMED` means observed or validated over all 490 supported
corpus cases; it does not imply unobserved constructs were dynamically tested.
`FIXTURE_CONFIRMED` means executable handwritten IR fixtures.

## The twelve contract points

### 1. Instruction-set closure

- Contract claim: only current emitter's closed textual subset, including
  select, zext/trunc, shifts, bitwise operators, call and two memory intrinsics.
- Actual observation: emitter switch emits these forms; focused manifest
  parsed and executed. Parser rejects unknown opcodes instead of guessing.
- Toy consumer requirement: no general LLVM fallback, no silently ignored syntax.
- **Status: FULL_CORPUS_CONFIRMED** (490 supported cases).

### 2. Entry-block alloca

- Contract claim: every alloca resides in the entry basic block.
- Actual observation: emitter formats alloca with alignment; fixtures and
  selected real tests ran. Parser rejects an alloca in any non-entry block.
- Toy requirement: fixed entry-time frame allocation, no dynamic RSP per inst.
- **Status: FULL_CORPUS_CONFIRMED** (490 supported cases).

### 3. Phi placement and contiguity

- Contract claim: all phi instructions form a contiguous block prefix.
- Observation: 256 corpus cases contain phi instructions; full-corpus parsing,
  validation, and execution passed. The malformed late-phi negative fixture is
  also rejected.
- Toy requirement: parse only the prefix; fail closed for later phi.
- **Status: FULL_CORPUS_CONFIRMED**.

### 4. Phi predecessor completeness

- Contract claim: one incoming value per CFG predecessor.
- Observation: validation compares predecessor sets and detects duplicate or
  missing inputs. A phi-cycle test executes correctly.
- Toy requirement: all incoming sources first copied to edge temporaries,
  then to destination SSA slots, including conditional edges.
- **Status: FULL_CORPUS_CONFIRMED**; corpus phis passed predecessor-set
  validation and execution, alongside the phi-cycle executable fixture.

### 5. GEP / index / address width

- Contract claim: 64-bit pointer, one i64 index, byte scale determined by
  sizeof(source_element_type).
- Observation: actual GEP emitter writes an i64 index; Flat pointer-to-pointer
  BitCast is textually rewritten as getelementptr i8, ptr ..., i64 0.
  A literal zero index is allowed; it need not be defined by sext.
  Array and byte-offset GEP executable fixtures passed.
- Toy requirement: typed scale, not a hardcoded 4; reject inbounds, i32
  indices and additional indices outside current subset.
- **Status: FULL_CORPUS_CONFIRMED** (396 cases contain GEP).
  **CONTRACT_DOC_STALE** for the historical requirement that all indices
  originate in sext; literal zero indices are valid.

### 6. Inbounds absence

- Contract claim: no inbounds GEP.
- Observation: emitter does not append inbounds; negative parser test rejects it.
- Toy requirement: exact shape, no speculative flags.
- **Status: FULL_CORPUS_CONFIRMED** (490 supported cases; `inbounds` count 0).

### 7. nsw/nuw/fast absence

- Contract claim: no these arithmetic modifiers.
- Observation: emitter writes bare opcodes. Negative nsw and fast fixtures
  reject unsupported syntax.
- Toy requirement: signed/unsigned 32-bit arithmetic wraps; no implicit flags.
- **Status: FULL_CORPUS_CONFIRMED** (490 supported cases; all three modifier
  counts are 0).

### 8. Zero-global representation

- Contract claim: zero globals are initialized explicitly and lowered to
  sparse BSS, while nonzero aggregates preserve element layout.
- Observation: appendGlobal emits zeroinitializer for empty data and nested
  typed array initializers otherwise. A 1.65-GB global fixture produces
  one .zero 1650000000 under .bss and a tiny assembly file.
- Toy requirement: no GB-sized buffers or expansion; .long words, sparse gaps,
  emitted alignment.
- **Status: FIXTURE_CONFIRMED**, actual emitter text inspected; the full corpus
  contained 1,128 `zeroinitializer` global definitions and all required cases
  passed, but sparse BSS behavior remains fixture-confirmed.

### 9. Array parameters as ptr

- Contract claim: opaque ptr parameters, not typed pointer-to-pointer syntax.
- Observation: emitter's llvmType prints ptr; parser accepts ptr params,
  disallows aggregate-valued params.
- Toy requirement: 64-bit SysV pointer ABI, no SysY AST shape metadata.
- **Status: FULL_CORPUS_CONFIRMED**; 152 corpus cases contain ptr function
  parameters (744 ptr parameters observed).

### 10. FCmp predicates

- Contract claim: only oeq, une, olt, ole, ogt, oge.
- Observation: toy parses only these predicates; executable tests check
  NaN unordered une versus ordered oeq and +0/-0 equality.
- Toy requirement: parity handling after ucomiss; not plain != for une.
- Actual corpus predicates: `oge` 116, `ogt` 41, `ole` 23, `olt` 194,
  `une` 138. `oeq` was not dynamically observed in the 490-case corpus; its
  behavior remains executable-fixture confirmed.
- **Status: FULL_CORPUS_CONFIRMED** for the observed predicate closure, with
  the `oeq` limitation above.

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
- **Status: FULL_CORPUS_CONFIRMED** (490/490 supported `case.ll` files define
  `main` as returning i32).

## Full-corpus observations

The independent run scanned **490 `case.ll` files**. Feature-bearing case
counts were: phi 256, alloca 476, GEP 396, i64 396, memcpy 14, and memset 44.
The observed `fcmp` predicates were `oge` 116, `ogt` 41, `ole` 23, `olt` 194,
and `une` 138. There were 0 `putf` calls and 0 external globals. Occurrences
of `inbounds`, `nsw`, `nuw`, and `fast` were all 0.

Main opcode occurrence highlights: `load` 59,467; `store` 38,235; `br` 41,741;
`icmp` 33,618; `getelementptr` 31,027; `sext` 30,983; `zext` 20,329;
`alloca` 17,593; `add` 21,022; `mul` 12,771; `sub` 10,627; `phi` 1,657;
`fcmp` 512; `select` 174.

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
- **VIOLATION**: none found in the accepted full-corpus run or focused controls.
- **CONTRACT_DOC_STALE**:
  - historical requirement that every GEP index originate from `sext`;
  - historical narrow four-context description of i64 use.
- **UNEXERCISED**:
  - actual-corpus `putf` call path (0 calls observed);
  - actual-corpus external-global path (0 external globals observed);
  - real target hardware performance;
  - accurate peak RSS measurement.
  Parser/unit fixtures do not upgrade these corpus paths to dynamically confirmed.
