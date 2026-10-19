// S16: affine dependence tests for one loop nest.
#ifndef SYSY_ANALYSIS_DEPENDENCEANALYSIS_H
#define SYSY_ANALYSIS_DEPENDENCEANALYSIS_H
#include <cstddef>
#include <cstdint>
#include <vector>
namespace sysy { namespace flat {
// One subscript: coeff * iv + constant. Multi-dimensional accesses provide one
// entry per loop dimension.
struct AffineExpr {
  int64_t coeff = 0;
  int64_t constant = 0;
};
struct LoopBounds {
  int64_t lower = 0;
  int64_t upper = 0;
};
enum class DependenceKind : uint8_t { None, Exact, Possible };
// `distance[d]` is sink iteration minus source iteration for an exact
// dependence, in the same dimension order as the access subscripts.
struct DependenceResult {
  DependenceKind kind = DependenceKind::None;
  std::vector<int64_t> distance;
  bool gcdTest = false;        // ran and did not prove independence
  bool banerjeeTest = false;   // ran and did not prove independence
  const char* reason = "empty";
};
// Equations: source[d](i) == sink[d](j). A pair is independent if any
// dimension has no integer solution (GCD) or the Banerjee range excludes zero.
// Exact results need a common coefficient in every dimension.
DependenceResult testDependence(const std::vector<AffineExpr>& source,
                                const std::vector<AffineExpr>& sink,
                                const std::vector<LoopBounds>& bounds);
}}  // namespace sysy::flat
#endif
