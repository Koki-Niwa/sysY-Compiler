#include "analysis/DependenceAnalysis.h"
#include <algorithm>
#include <cstdlib>
#include <numeric>
namespace sysy { namespace flat {
namespace {
int64_t gcd64(int64_t a, int64_t b) {
  a = std::llabs(a);
  b = std::llabs(b);
  while (b != 0) {
    const int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}
// Range of `sourceCoeff * i - sinkCoeff * j + offset` for i,j in [lower,upper].
struct Interval {
  int64_t lo;
  int64_t hi;
};
Interval extreme(int64_t coefficient, const LoopBounds& bounds) {
  const int64_t a = coefficient * bounds.lower;
  const int64_t b = coefficient * bounds.upper;
  return Interval{std::min(a, b), std::max(a, b)};
}
}  // namespace
DependenceResult testDependence(const std::vector<AffineExpr>& source,
                                const std::vector<AffineExpr>& sink,
                                const std::vector<LoopBounds>& bounds) {
  DependenceResult result;
  if (source.empty() || source.size() != sink.size() ||
      bounds.size() != source.size()) {
    result.kind = DependenceKind::None;
    result.reason = "dimension mismatch";
    return result;
  }
  bool exact = true;
  for (size_t d = 0; d < source.size(); ++d) {
    const AffineExpr& src = source[d];
    const AffineExpr& dst = sink[d];
    const int64_t offset = dst.constant - src.constant;
    if (src.coeff != dst.coeff) exact = false;
    // GCD: a solution of srcCoeff*i - dstCoeff*j = offset needs the gcd to
    // divide the offset.
    const int64_t g = gcd64(src.coeff, dst.coeff);
    if (g == 0) {
      if (offset != 0) {
        result.reason = "gcd (both constant)";
        return result;
      }
      result.distance.push_back(0);   // identical constants: same location
      continue;
    }
    if (offset % g != 0) {
      result.reason = "gcd";
      return result;
    }
    result.gcdTest = true;
    // Banerjee range test over the iteration domain.
    const LoopBounds& b = bounds[d];
    const Interval si = extreme(src.coeff, b);
    const Interval sj = extreme(dst.coeff, b);
    const int64_t lo = si.lo - sj.hi + offset;
    const int64_t hi = si.hi - sj.lo + offset;
    if (lo > 0 || hi < 0) {
      result.reason = "banerjee";
      return result;
    }
    result.banerjeeTest = true;
    if (src.coeff == dst.coeff) {
      const int64_t coefficient = src.coeff;
      if (coefficient == 0) continue;
      result.distance.push_back(offset / coefficient);
    }
  }
  if (exact) {
    result.kind = DependenceKind::Exact;
    result.reason = "exact single subscript";
    return result;
  }
  result.kind = DependenceKind::Possible;
  result.distance.clear();
  result.reason = "banerjee/gcd possible; distance not exact";
  return result;
}
}}  // namespace sysy::flat
