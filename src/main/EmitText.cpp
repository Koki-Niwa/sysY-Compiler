// ============================================================================
// main/EmitText.cpp —— `--emit=*` 的**渲染实现**（从 `main.cpp` 切出）
//
//   为什么单独一个文件：`main.cpp` 是命令行入口 + 各档产物的流水线装配，
//   而"把某一档渲染成文本"（`--help` 文本）与命令行解析无关。
//   §C4 的单文件行数上限（600）要求把它切出去。
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

}  // namespace mainstage
}  // namespace sysy
