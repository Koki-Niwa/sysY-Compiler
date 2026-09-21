// ============================================================================
// StructuredVerifier —— 六条不变式的**检查器**（S05 交付物 #4）
//
// 设计文档 §2 / prompt §4.2 的六条：
//
//   I1  ForOp 的 IV 在循环体内不被赋值          —— ★ S05 不产生 ForOp，**平凡成立**
//   I2  ForOp 的 lower/upper/step 在体内不被修改 —— ★ 同上
//   I3  ForOp 体内没有 break（SCoP 条件）       —— ★ 同上
//   I4  每个 Region **恰好一个终结 Op**，且在最后一行 —— ★ **S05 必须自己保证**
//   I5  不存在不可约控制流（控制流容器只有 If/While/For）
//   I6  每个数组下标访问都被标记为仿射/非仿射
//
// ⚠️ **I1–I3 在 S05 的处境必须说清楚**：S05 的 IRGen **不产 ForOp**（循环
//    归一化是 S05b），所以这三条在本关是"**没有 ForOp 所以平凡成立**"。
//    检查器**照实现**（规格要求），并在"手工构造的 IR"上有单元断言；
//    但**不要**把"检查器实现了 I1–I3"误读成"S05 验证过循环不变量"——
//    真正的验证在 S05b。这条已写进报告。
//
// ── 为什么检查器与 IRGen 是两份代码而不是"IRGen 顺手断言" ────────────────
//   同一个 bug 会同时改坏"生成"和"自检"，于是自检永远通过（S04 的 `--probe`
//   就是为这件事加的）。所以这里独立遍历、独立判定；`check_structured.py`
//   是**第三份**（从文本出发，完全不看这个文件）。
// ============================================================================
#ifndef SYSY_STRUCTURED_STRUCTUREDVERIFIER_H
#define SYSY_STRUCTURED_STRUCTUREDVERIFIER_H

#include <string>
#include <vector>

#include "structured/StructuredIR.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace sir {

// 一条违反记录：不变式编号（I1..I6）、位置、人类可读的说明。
struct Violation {
  const char* invariant = "";
  SourceLoc loc;
  std::string message;
};

// 【前置】module == nullptr 或 module->kind == OpKind::Module。
// 【后置】返回全部违反（空 = 全部通过）。**不抛异常、不 assert**：
//         检查器的主要输入是"可能被改坏的 IR"（单元测试与 `--probe`），
//         遇到畸形结构要报出来而不是崩（C5）。
//         遍历是**显式工作栈**（与 IRGen 的表达式遍历同理）。
std::vector<Violation> verifyModule(const Op* module);

// 【后置】把违反列表渲染成多行文本（每条一行，便于 diff 与报告粘贴）。
std::string formatViolations(const std::vector<Violation>& vs);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_STRUCTUREDVERIFIER_H
