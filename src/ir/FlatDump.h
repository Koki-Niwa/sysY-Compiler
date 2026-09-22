// ============================================================================
// ir/FlatDump.h —— 平面 IR 的**文本 dump + 读回**（S06 交付物 #4）
//
// 规格：`docs/handoff/03-设计/平面IR与dump格式.md`（§2–§6）。
//
// ── 三条硬性质（prompt §四）──────────────────────────────────────────────
//   ① **纯函数**：同输入两次运行逐字节相同（不含绝对路径/时间戳/指针地址）；
//   ② **可读回且逐字节往返**：`--emit=flat-ir → --from-flat → --emit=flat-ir`
//      与原文**逐字节相同**（轨 A）；
//   ③ **真实输出定义排版**：`compiler/tests/flat/example/` 的样例对由
//      `--emit=flat-ir` **真实运行产出**，关卡用 `cmp -s` 冻结它。
//      ⚠️ "手写例子当契约"这条规矩本项目踩过三次（S03/S04/S05），所以
//         dump 的排版**只由本文件的实现决定**，样例对是它的产物。
//
// ── 打印/识别必须**逐字对应**（与 S05 的 StructuredDump/Reader 同一纪律）──
//   本文件的 `appendValue`/`appendInst` 与 FlatReader.cpp 的 `parseInst`
//   是一对互逆函数；改一处必须改另一处，且轨 A 会立刻抓到不一致。
//
// ── 排版（规则见设计文档；这里只记最易错的三条）──────────────────────────
//   * 缩进 2 空格（指令）/ 0 空格（标签、`define`、全局）；
//   * 每个**无名常量**在**首次被引用**时拿到一个编号，并在**该函数的
//     常量区**（`; consts` 之前）打印它的定义行 —— 见下；
//   * `phi` 的入值写成 `(%v L3)`，**前驱块必须显式写出**（S08 的 SSA 检查器
//     与 S09 的 mem2reg 都要用它）。
//
// ── 常量怎么打印（一个**必须写死**的排版决定）────────────────────────────
//   常量**不内联**（不用 `i32 5` 当操作数），而是给编号、单独一行：
//       %7 = i32 5 @line 12
//   理由与 S05 完全一致（那次是"内联常量"被实测推翻两次）：**统一规则**让
//   "往返同构"成为结构性质 —— 读回器不需要"猜"哪个操作数该现场造一个常量。
//   常量的定义行插在**它所属函数的第一条指令之前**（按编号升序），
//   于是"定义在前、使用在后"在文本上可见（这也是 LLVM 的阅读习惯）。
// ============================================================================
#ifndef SYSY_IR_FLATDUMP_H
#define SYSY_IR_FLATDUMP_H

#include <string>

#include "ir/Module.h"
#include "support/Diagnostic.h"

namespace sysy {
namespace flat {

// 【前置】m 的 use-def / preds 已重建（`rebuildCFG()` + `rebuildUseDef()`）。
// 【后置】返回整个模块的文本 dump（**纯函数**：同一 m 多次调用逐字节相同）。
std::string dumpModule(const Module& m);

// 【前置】text 是 `dumpModule` 的产物（或任意畸形文本 —— 读回要能报错，
//         不许崩，C5）。
// 【后置】成功 → 返回新模块（调用方持有所有权；其中所有对象由它释放），
//         并已调过 `rebuildCFG()` + `rebuildUseDef()`；
//         失败 → 返回 nullptr，诊断已报进 `diags`（尽量多报，不早退）。
Module* parseFlatModule(const std::string& text, DiagnosticEngine& diags);

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_FLATDUMP_H
