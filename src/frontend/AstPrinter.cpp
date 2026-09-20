// ============================================================================
// AstPrinter.cpp —— S-表达式打印器（`--emit=ast` 的实现）
//
// ── 本文件现在只是**薄壳**（S03 起）─────────────────────────────────────
//   布局逻辑（缩进、括号落位、子节点顺序、迭代工作栈）已经搬到
//   `AstSexpLayout.h` 的 `SexpLayout<Anno>` 模板里，因为 `--emit=sema`
//   必须"只做加法"地复用同一份布局：
//
//       --emit=ast   → SexpLayout<NoAnno>    （注解全是空实现）
//       --emit=sema  → SexpLayout<SemaAnno>  （只追加类型注解）
//
//   验收里的"去注解还原"判据（prompt §六 第 4 条）就是这条复用的机械检验：
//   把 sema 转储剥掉注解后必须与 ast 转储**逐字节相同**。
//   ⇒ **不要**在这里或 SemaDump.cpp 里另写一份布局。
//
//   ⚠️ `--emit=ast` 的输出是 S02 冻结的对外契约：改这里等于改契约，
//      门禁是 `tools/selftest/baseline_ast_sha256.txt`（490 个文件的 SHA-256）
//      与 `tools/selftest/check_parser.py`（490 个往返）。
//
//   格式的历史注记、深度约束的不对称性（打印器迭代 vs 读取器递归）、
//   以及"为什么右括号全堆在最后一行"，见 AstSexpLayout.h 与 AstPrinter.h。
// ============================================================================
#include "frontend/AstPrinter.h"

#include <string>

#include "frontend/AstSexpLayout.h"

namespace sysy {

std::string printAst(const CompUnit& unit) {
  sexp::SexpLayout<sexp::NoAnno> layout;
  return layout.run(unit);
}

}  // namespace sysy
