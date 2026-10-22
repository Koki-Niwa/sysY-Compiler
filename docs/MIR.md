# Standalone MIR prototype

`src/mir/` is an independent B01 experiment. The compiler CLI does not lower
Flat IR into it, and it emits no machine code. The formal target backend
requirements in `BACKEND-PERFORMANCE-PLAN.md` remain open: multiple results,
physical register units and constraints, calls, target assignment and final
frame layout are not implemented by this prototype.

## Representation and validation

Functions own contiguous, zero-based blocks. Virtual register IDs are nonzero,
unique within a function and carry a consistent type/class. Phi definitions
form a leading group and take their inputs simultaneously on the selected
predecessor edge. Ordinary uses must follow a dominating definition.

Opcode semantics have one internal authority shared by the reader, dump and
verifier. Each current opcode defines its spelling, definition policy,
operand shape, type relationships, memory form and terminator status. A parsed
module is not implicitly verified: callers must call `verify` before analysis
or transformation. Invalid instruction shapes must be rejected before indexed
CFG or dominance checks consume them.

The reader checks complete numeric tokens and the destination field's range.
Malformed or overflowing input returns `nullopt` and a diagnostic instead of
throwing a numeric-conversion exception or truncating an integer. Supported
valid dumps retain deterministic parse/dump round trips.

Memory facts belong to load/store instructions. Register-based memory uses
an integer GPR base; the legacy base ID zero explicitly represents an
unspecified address and is not an SSA use. Frame-based memory instead names a
declared frame object. If `frameObjects` is nonempty it defines the frame
namespace and object sizes; otherwise `frameSlots` defines a legacy namespace
with unknown object sizes. Both forms require an in-range frame index and a
nonzero access size. Known-size objects additionally require the entire access
to fit the object, using checked offset/size arithmetic. Legacy counts cannot
prove byte bounds and must not be used to invent an object size.

## Liveness and allocation experiments

`analyzeLiveness` computes a monotone fixed point over block live-in/live-out
sets, with incoming phi uses attached only to the corresponding CFG edge.
Memory base registers participate as ordinary uses. `buildInterference` walks
instructions backward from live-out, connecting definitions to values live
after them before processing their inputs. Phi inputs are never block-local
uses; the leading phi definitions are handled as a parallel group. This is
necessary even for a function consisting of one block with empty boundary
live sets.

`greedyColor` colors this virtual-register graph deterministically, with copy
hints and explicit spill marks. Colors are abstract locations, not physical
registers. RegMask facts round-trip, but the allocator does not implement
target clobber constraints or register-bank assignment. No real target
allocation quality or performance is claimed.

## Spill rewrite boundary

`planSpills` describes new spill objects; it does not extend an existing frame.
`rewriteSpills` currently accepts only functions with `frameSlots == 0` and
empty `frameObjects`. Existing storage is rejected before mutation, including
legacy counts whose sizes are unknown. This explicit limitation prevents
spill object zero from aliasing an existing local object.

Rewriting validates the input and plan, stages edge reloads and replacement
instructions in scratch storage, and commits only a valid complete result.
Failure leaves the caller's function unchanged. A phi edge reload is placed
before its predecessor's terminator, including on a self-backedge; building
that reload must not invalidate references into a traversed vector. Stores of
spilled phi results follow the entire leading phi group. Fresh virtual IDs
must not wrap, and each spill access must fit its planned object.

Every successful edit invalidates previously computed liveness, interference
and allocation information. These objects have no revision-aware ownership in
this prototype: callers must explicitly recompute them. Work is bounded by the
finite input and planned reload/store expansion; there is no iterative
allocation/spill fixed point. Scratch construction adds a function-sized
temporary copy rather than claiming an in-place rollback mechanism.

## Regression evidence

The MIR tests exercise single-block interference, dead definitions, memory
bases, phi edges, self-loop spilling, simultaneous phi groups, invalid plans,
frame collisions and parser/verifier rejection boundaries. The repair tests
must also run against the pre-repair source as negative controls, with valid
neighboring fixtures retained. Sanitizer validation covers the self-edge
vector-invalidation reproducer. Full S09 and Clang CTest remain required;
their results do not establish a functioning machine-code backend.
