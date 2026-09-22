// ============================================================================
// main/EmitText.cpp —— `--emit=*` 的**渲染实现**（从 `main.cpp` 切出）
//
//   为什么单独一个文件：`main.cpp` 是命令行入口 + 各档产物的流水线装配，
//   而"把某一档渲染成文本"（占位产物、`--help` 文本）与命令行解析无关。
//   §C4 的单文件行数上限（600）要求把它切出去。
//   ★ 本文件**不改变任何行为**：函数体逐字搬自 `main.cpp`（只换了命名空间）。
// ============================================================================
#include "main/EmitText.h"

#include <string>

namespace sysy {
namespace mainstage {

std::string helpText() {
  return
      "SysY compiler (front/middle end) —— SysY -> LLVM IR subset\n"
      "\n"
      "用法: compiler <input> -o <output> [选项]\n"
      "\n"
      "选项:\n"
      "  -o <file>          输出文件（必填）\n"
      "  --emit=<kind>      产物类型: llvm-ir（默认）| tokens | ast | sema | initplan\n"
      "                     | structured-ir | flat-ir | nothing\n"
      "  --from-ast         输入是 --emit=ast 的产物（S-表达式），而不是 SysY 源码\n"
      "                     （S02 起用于验证：source --emit=ast --from-ast --emit=ast）\n"
      "                     --emit=sema 时同样接受；产物不保证能被读回（单向视图）\n"
      "  --from-structured  输入是 --emit=structured-ir 的产物（S-表达式），而不是\n"
      "                     SysY 源码（S05 起用于轨 A：往返必须逐字节相同）\n"
      "  --from-flat        输入是 --emit=flat-ir 的产物（S06 起用于轨 A）\n"
      "  --dump-flat-stats  把平面 IR 的统计（块/指令/φ）打到 stderr\n"
      "  -O0 / -O1          优化级别（-O1 目前只记录，S17 起生效）\n"
      "  --optimize         -O1 的别名（tools/run_tests.sh 使用长选项名）\n"
      "  -S                 接受但不解释（比赛调用形式: compiler a.sy -S -o a.s）\n"
      "  --structured       使用结构化 IR 层（默认）\n"
      "  --no-structured    不走结构化 IR 层（供回归对照用）\n"
      "  --normalize        跑 LoopNormalize（while→for + continue 消解）与\n"
      "                     AllocaHoist（alloca 提到函数入口 Region）。**默认关**：\n"
      "                     不带它时 --emit=structured-ir 的输出与 S05 契约逐字节相同\n"
      "  --dump-loopnorm-stats  把规范化统计（被规范化/保留的循环、原因直方图）\n"
      "                     打到 stderr（进不了 dump：dump 是冻结契约）\n"
      "  --toy-backend      接受但不解释（测试链路选择：走自研降级器而非 clang）\n"
      "  --verbose          打印处理过程\n"
      "  -h, --help         显示本帮助并退出\n"
      "  -v, --version      显示版本并退出\n"
      "\n"
      "输入扩展名 .sy 与 .sysy 都接受。\n"
      "\n"
      "退出码: 0 成功 | 1 编译错误 | 2 命令行用法错误 | 3 文件读写错误\n";
}

std::string placeholderText(const std::string& emitName, const std::string& inputPath,
                            size_t lineCount, bool optLevel1, bool structured) {
  std::string out;
  out += "; SysY compiler (front/middle end) —— stage S03 (type system + sema)\n";
  out += "; input:  " + inputPath + "\n";
  out += "; emit:   " + emitName + "\n";
  out += "; lines:  " + std::to_string(lineCount) + "\n";
  out += std::string("; -O1:    ") +
         (optLevel1 ? "requested (not implemented yet)" : "off") + "\n";
  out += std::string("; structured IR layer: ") + (structured ? "on" : "off") + "\n";
  out += ";\n";
  out += "; 本阶段真实可用：--emit=tokens（词法）、--emit=ast / --from-ast（语法+AST）、\n";
  out += ";               --emit=sema（语义分析 + 类型注解转储）、\n";
  out += ";               --emit=initplan（初始化计划）、\n";
  out += ";               --emit=structured-ir / --from-structured（结构化 IR）、\n";
  out += ";               --emit=flat-ir / --from-flat（平面 IR，S06）。\n";
  out += "; LLVM IR 文本发射器尚未实现（S07）；本档是占位。\n";
  out += "; 交给后端的 .ll 契约（TESTING-GUIDE §3.3）：目标无关 ——\n";
  out += ";   不带 target triple、不带 target datalayout、\n";
  out += ";   函数属性里不带 target-cpu / target-features。\n";
  out += "; 目标由降级阶段（llc -mtriple / 自研后端）决定。\n";
  return out;
}

}  // namespace mainstage
}  // namespace sysy
