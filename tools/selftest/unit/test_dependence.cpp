// S16: hand-computed affine dependence cases.
#include <cstdio>
#include <string>
#include <vector>
#include "analysis/DependenceAnalysis.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
AffineExpr expr(int64_t coeff, int64_t constant = 0) {
  return AffineExpr{coeff, constant};
}
LoopBounds bounds(int64_t lo = 0, int64_t hi = 9) {
  return LoopBounds{lo, hi};
}
void expectExact(const char* what, std::vector<AffineExpr> source,
                 std::vector<AffineExpr> sink, std::vector<LoopBounds> domain,
                 std::vector<int64_t> distance) {
  const DependenceResult result = testDependence(source, sink, domain);
  const bool ok = result.kind == DependenceKind::Exact &&
                  result.distance == distance && result.gcdTest &&
                  result.banerjeeTest;
  check(ok, what);
  if (!ok) std::printf("  got kind=%d distance=%zu expected=%zu\n",
                       static_cast<int>(result.kind), result.distance.size(),
                       distance.size());
}
void testCases() {
  expectExact("A[i] -> A[i] is distance 0", {expr(1)}, {expr(1)},
              {bounds()}, {0});
  expectExact("A[i] -> A[i+1] is distance 1", {expr(1)}, {expr(1, 1)},
              {bounds()}, {1});
  expectExact("A[i+1] -> A[i] is distance -1", {expr(1, 1)}, {expr(1)},
              {bounds()}, {-1});
  expectExact("A[2i] -> A[2i+2] is distance 1", {expr(2)}, {expr(2, 2)},
              {bounds()}, {1});
  expectExact("A[4i+1] -> A[4i+5] is distance 1", {expr(4, 1)}, {expr(4, 5)},
              {bounds()}, {1});
  expectExact("2-D A[i][j] -> A[i][j+1] is {0,1}",
              {expr(1), expr(1)}, {expr(1), expr(1, 1)},
              {bounds(), bounds()}, {0, 1});
  {
    const DependenceResult r =
        testDependence({expr(2)}, {expr(2, 1)}, {bounds()});
    check(r.kind == DependenceKind::None && r.reason != nullptr &&
          std::string(r.reason) == "gcd",
          "A[2i] -> A[2i+1] has no integer solution");
  }
  {
    const DependenceResult r =
        testDependence({expr(2)}, {expr(3)}, {bounds()});
    check(r.kind == DependenceKind::Possible && r.gcdTest && r.banerjeeTest &&
          r.distance.empty(), "A[2i] -> A[3i] is possible with unknown distance");
  }
  {
    const DependenceResult r =
        testDependence({expr(1)}, {expr(1, 100)}, {bounds()});
    check(r.kind == DependenceKind::None && std::string(r.reason) == "banerjee",
          "A[i] -> A[i+100] is outside the bounded domain");
  }
  {
    const DependenceResult r =
        testDependence({expr(1), expr(2)}, {expr(1), expr(2, 1)},
                       {bounds(), bounds()});
    check(r.kind == DependenceKind::None && std::string(r.reason) == "gcd",
          "second dimension can prove independence");
  }
  {
    const DependenceResult r = testDependence({expr(0, 4)}, {expr(0, 4)},
                                              {bounds()});
    check(r.kind == DependenceKind::Exact && r.distance == std::vector<int64_t>{0},
          "constant identical subscripts are distance 0");
  }
  {
    const DependenceResult r = testDependence({expr(0)}, {expr(0, 1)}, {bounds()});
    check(r.kind == DependenceKind::None && std::string(r.reason) == "gcd (both constant)",
          "distinct constants never alias");
  }
  {
    const DependenceResult r = testDependence({expr(1)}, {expr(1, 1)}, {});
    check(r.kind == DependenceKind::None && std::string(r.reason) == "dimension mismatch",
          "missing bounds fail closed");
  }
}
}  // namespace
int main() {
  testCases();
  std::printf("S16_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
