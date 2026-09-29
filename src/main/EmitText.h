// ============================================================================
// main/EmitText.h —— `--emit=*` 的渲染文本（`--help`）
//   实现见 EmitText.cpp。
// ============================================================================
#ifndef SYSY_MAIN_EMITTEXT_H
#define SYSY_MAIN_EMITTEXT_H

#include <string>

namespace sysy {
namespace mainstage {

// 【后置】`--help` 的完整文本。
std::string helpText();

}  // namespace mainstage
}  // namespace sysy
#endif  // SYSY_MAIN_EMITTEXT_H
