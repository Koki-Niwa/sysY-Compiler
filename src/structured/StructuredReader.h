// ============================================================================
// StructuredReader —— 结构化 IR 文本 → 容器（`dumpModule` 的逆函数）
//
//   ★ 存在的唯一理由：**轨 A**（prompt §七）——
//     `--emit=structured-ir` → `--from-structured --emit=structured-ir`
//     → **逐字节相同**。它证明"打印器与读取器互逆"（S02 的轨 A 抓过
//     "打印两次结果不同"的真 bug）。
//
// ── 它是**忠实**的读回器，不是优化器 ─────────────────────────────────────
//   读回来的 IR 与原文一一对应：同样的 Op 顺序、同样的结果编号规则、
//   同样的属性顺序。唯一的"重建"是按 OpKind 推出**结果类型**（文本里只打印了
//   结果名，类型由 Op 的语义决定）—— 那也是为了避免让 dump 体积翻倍。
//
// ── 解析策略：先把文本**规范化成词法单元**，再按 Op 逐行解析 ──────────────
//   ① 逐行扫描：丢掉缩进，提取"这一行是不是 `{` / `}` / `(OpKind ...)`
//      （可能以 ` {` 结尾）"；
//   ② 每个 Op 行内的内容按小词法单元切开（`(` `)` `%` `[` `]` `,` `=`、
//      字符串、数字、类型名、`@line`）—— **不用正则**，零第三方依赖；
//   ③ 按 OpKind 的"头部语法"吃 tokens（结果 → 头部属性 → 操作数 → 尾部属性）。
//
// ── 为什么要按 OpKind 定"结果个数"而不是靠猜 ─────────────────────────────
//   名字（`%x`）既可能是结果、也可能是操作数引用（`YieldOp %x`）。靠"位置"
//   猜会在 `Yield`/`Call`（可 void）上出错。所以：
//     * `IfOp` **恒有 1 个结果**（prompt §4.1 冻结 0..N 的容器语义不变；
//       IRGen 生成的语句级 if 只是"结果未被使用"）；
//     * `CallOp` 由**被调函数**是否 void 决定（运行时的 void 函数在名字表里）；
//     * 其余 OpKind 的结果个数**固定**（0 或 1）。
//   于是"读回"是**可判定**的，不依赖任何启发式。
// ============================================================================
#ifndef SYSY_STRUCTURED_STRUCTUREDREADER_H
#define SYSY_STRUCTURED_STRUCTUREDREADER_H

#include <string>

#include "structured/StructuredIR.h"

namespace sysy {

class DiagnosticEngine;

namespace sir {

// 【前置】text 是 `dumpModule` 形态的 S-表达式文本（可能畸形）。
// 【后置】成功 → 返回 ModuleOp（新分配的 Op 都在 arena 里）；
//         失败 → 报**至少一条**诊断并返回 nullptr（绝不静默返回空模块）。
//         不抛异常（畸形输入不崩，C5）。
Op* parseStructuredModule(const std::string& text, Arena& arena, DiagnosticEngine& diag);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_STRUCTUREDREADER_H
