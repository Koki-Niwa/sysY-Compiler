#include <cstdio>
#include <string>

#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "ir/FlatDump.h"
#include "structured/AllocaHoist.h"
#include "structured/FlattenCFG.h"
#include "structured/IRGen.h"
#include "structured/LoopNormalize.h"
#include "structured/StructuredDump.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  std::printf("  %s %s\n", condition ? "\u2714" : "\u2718", message);
  if (!condition) ++failures;
}

size_t count(const std::string& text, const std::string& needle) {
  size_t result = 0;
  for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos;
       at += needle.size()) {
    ++result;
  }
  return result;
}

void testFptosiBoundaries() {
  const std::string source =
      "int main(){float upper=0x1p+31;float below=0x1.fffffep+30;"
      "float lower=-0x1p+31;int a=upper;int b=below;int c=lower;"
      "if(a!=2147483647)return 1;if(b!=2147483520)return 2;"
      "if(c!=(-2147483647-1))return 3;return 0;}";
  sysy::SourceFile file = sysy::SourceFile::fromString("fptosi_boundary.sy", source);
  sysy::DiagnosticEngine diags;
  diags.setSourceFile(&file);
  sysy::Lexer lexer(file, diags);
  sysy::Parser parser(lexer, diags);
  auto unit = parser.parseCompUnit();
  expect(unit != nullptr, "边界源码能解析");
  if (unit == nullptr) return;

  sysy::Sema sema(diags);
  sema.run(*unit);
  const sysy::InitPlan plan = sysy::runInitLowering(*unit, sema);
  sysy::sir::Arena arena;
  sysy::sir::Op* structured =
      sysy::sir::buildModule(arena, *unit, plan, "fptosi_boundary.sy", diags);
  const std::string structuredDump = sysy::sir::dumpModule(structured);
  expect(count(structuredDump, "(Ge ") == 3,
         "structured：三次转换的上界守卫均为 Ge");
  expect(count(structuredDump, "(Gt ") == 0,
         "structured：精确 2^31 不再使用 Gt 守卫");

  sysy::sir::LoopNormStats stats;
  structured = sysy::sir::normalizeLoops(structured, arena, stats);
  sysy::sir::hoistAllocas(structured);
  sysy::flat::Module flat;
  expect(sysy::flat::flattenModule(structured, flat, diags) != nullptr &&
             !diags.hasError(),
         "真实 frontend→structured→flat 流水线成功");
  const std::string flatDump = sysy::flat::dumpModule(flat);
  expect(count(flatDump, "fcmp oge f32") == 6,
         "flat：外层守卫与 F2I 降级都使用 oge");
  expect(count(flatDump, "fcmp ogt f32") == 0,
         "flat：精确 2^31 不会选中越界 fptosi 结果");
  expect(flatDump.find("f32 0x1.fffffep+30") != std::string::npos,
         "覆盖 2^31 的相邻下界 f32");
  expect(flatDump.find("f32 -0x1p+31") != std::string::npos &&
             count(flatDump, "fcmp olt f32") == 6,
         "覆盖精确下界 -2^31，下界守卫仍为严格 olt");
}

}  // namespace

int main() {
  std::printf("═══ float→int 饱和边界回归 ═══\n");
  testFptosiBoundaries();
  std::printf("失败 %d 项\n", failures);
  return failures == 0 ? 0 : 1;
}
