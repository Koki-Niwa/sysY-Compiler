#include <cstdio>

#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "ir/FlatVerifier.h"
#include "structured/AllocaHoist.h"
#include "structured/FlattenCFG.h"
#include "structured/IRGen.h"
#include "structured/LoopNormalize.h"
#include "support/SourceFile.h"

int main() {
  const char* src = R"(
int k;
int main() {
  k = 3389;
  if (k < 10000) {
    k = k + 1;
    int k = 112;
    while (k > 10) {
      k = k - 88;
      if (k < 1000) {
        int g = 9;
        { int l = 11; { g = 10; k = k - g; int g = 11; k = k + g + l; } }
      }
    }
    putint(k);
  }
  return k;
}
)";
  sysy::SourceFile file = sysy::SourceFile::fromString("dead_phi.sy", src);
  sysy::DiagnosticEngine diags;
  diags.setSourceFile(&file);
  sysy::Lexer lexer(file, diags);
  sysy::Parser parser(lexer, diags);
  auto unit = parser.parseCompUnit();
  if (unit == nullptr || diags.hasError()) return 1;

  sysy::Sema sema(diags);
  sema.run(*unit);
  const sysy::InitPlan plan = sysy::runInitLowering(*unit, sema);
  sysy::sir::Arena arena;
  sysy::sir::Op* smod = sysy::sir::buildModule(arena, *unit, plan, "dead_phi.sy", diags);
  sysy::sir::LoopNormStats stats;
  smod = sysy::sir::normalizeLoops(smod, arena, stats);
  sysy::sir::hoistAllocas(smod);

  sysy::flat::Module flat;
  if (sysy::flat::flattenModule(smod, flat, diags) == nullptr || diags.hasError()) return 1;
  const sysy::flat::Function* mainFn = flat.findFunction("main");
  const auto violations = sysy::flat::verifyFlatModule(flat);
  if (mainFn == nullptr || mainFn->phiCount() != 0 || !violations.empty()) {
    std::printf("dead-phi regression: phis=%zu violations=%zu\n",
                mainFn == nullptr ? 0 : mainFn->phiCount(), violations.size());
    return 1;
  }
  std::puts("  \u2714 scope-shadowing loop leaves no unobserved phi cycle");
  return 0;
}
