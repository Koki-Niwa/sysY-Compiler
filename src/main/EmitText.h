// ============================================================================
// main/EmitText.h —— `--emit=*` 的渲染文本（`--help` 与占位产物）
//   实现见 EmitText.cpp（从 main.cpp 切出，行为不变）。
// ============================================================================
#ifndef SYSY_MAIN_EMITTEXT_H
#define SYSY_MAIN_EMITTEXT_H

#include <cstddef>
#include <string>

namespace sysy {
namespace mainstage {

// 【后置】`--help` 的完整文本。
std::string helpText();

// 【后置】"尚未实现的 emit 档位"的占位产物（保持与 S03 起的形态一致）。
std::string placeholderText(const std::string& emitName, const std::string& inputPath,
                            size_t lineCount, bool optLevel1, bool structured);

}  // namespace mainstage
}  // namespace sysy
#endif  // SYSY_MAIN_EMITTEXT_H
