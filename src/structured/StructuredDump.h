// ============================================================================
// StructuredDump —— 结构化 IR 的**文本格式**（S05 交付物 #5）
//
// ★ 本文件是 S05 **对外契约**的唯一定义处（关卡用 `cmp -s` 冻结
//   `tools/selftest/structured_example/`）。S06 的 `FlattenCFG` 按它读。
//
// ── 为什么"先写规则、再让真实输出定义排版"（prompt §六）──────────────────
//   前两关踩过两次：S03 的转储"例子与规则文字自相矛盾"，S04 的示例与实际输出
//   有三处排版差异。两次都是关卡里的样例对比抓出来的。⇒ 本关的规矩是：
//   **规则写在下面，样例对由真实运行产出**，两者不一致时以样例对为准。
//
// ── 格式规则（5 条，与 prompt §六 逐条对应）───────────────────────────────
//   ① S-表达式风格，**一行一个 Op**，缩进 2 空格/层；Region 用 `{ ... }`
//      包起来（`{` 在被拥有者那一行的末尾，`}` 独占一行、与拥有者同缩进）。
//   ② 一行之内依次是：`(OpKind` → 结果列表 → 操作数 → 属性 → `)`。
//      * **结果**一律命名 `%<函数名>.<序号>`（如 `%main.7`），**必须打印** ——
//        没有它，"0..N 结果"这件事在文本里就看不出来。无结果的 Op 不打印名字
//        （`StoreOp` 与"有两个结果的 Op"因此一眼可辨）。
//      * **操作数**引用同一个命名，或写成带类型的常量：`i32 5` / `f32 0x1.4p+3`。
//   ③ **函数头带签名**：`(Func main :ret i32)` / `(Func f :ret f32 :param [ptr[i32], i32])`，
//      因为 S06 要按它分配。
//   ④ 每个 Op 打印 `@line <n>`（D11：SourceLoc 一路带到 IR；`starttime` 依赖它）。
//      行号是**纯函数**的一部分（同一输入两次运行逐字节相同），不含绝对路径/
//      时间戳/随机数/指针地址。
//   ⑤ 转储**必须能被读回**（`--from-structured`），且读回后再 dump **逐字节相同**。
//
// ── 遍历必须显式工作栈（prompt §九：遍历与打印不许递归）──────────────────
//   S02 的打印器递归版在 6 万个 `+` 的链上 rc=139（SIGSEGV）。IR 的树深虽然
//   远小于 AST（86_long_code2 的 AST 树深 4007，但它是一条**表达式**链，
//   IR 里那是 4000 条平铺的 Op，不是 4000 层嵌套），这里仍然用显式栈：
//   "不崩"是硬验收，而不是"估计不会崩"。
// ============================================================================
#ifndef SYSY_STRUCTURED_STRUCTUREDDUMP_H
#define SYSY_STRUCTURED_STRUCTUREDDUMP_H

#include <string>

#include "structured/StructuredIR.h"

namespace sysy {
namespace sir {

// 【前置】module != nullptr 且 module->kind == OpKind::Module。
// 【后置】返回完整的 S-表达式转储（以换行结尾）。**纯函数**：
//         同一棵树两次调用逐字节相同；不含路径/时间戳/地址。
//         这是 `--emit=structured-ir` 的唯一实现。
std::string dumpModule(const Op* module);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_STRUCTUREDDUMP_H
