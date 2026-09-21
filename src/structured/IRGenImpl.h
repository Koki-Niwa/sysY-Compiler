// ============================================================================
// IRGenImpl.h —— Sema 类型 → 结构化 IR 类型的翻译（不对外）
//   ⚠️ 这里不做语义判断：类型是 Sema 填好的 —— 本文件只负责"换个表示"。
//
// ── 为什么叫 `toIrType` 而不是 `irTypeOf`（一个实测踩过的坑）──────────────
//   `sysy::Type`（Sema 的类型）与 `sir::Type`（结构化 IR 的类型）**同名**。
//   如果这里的函数也叫 `irTypeOf` 并放在 `sysy` 命名空间，那么 `sir` 内部的
//   非限定调用 `irTypeOf(x)` 会做**重载解析**，而 `sysy::Type*` 在 `sir` 里
//   是"另一个类型的指针" —— 实测报的是"cannot convert"，看起来像类型写错了。
//   ⇒ 换个不会撞的名字，并且**只声明在 `sir` 里**（定义在 IRGenTypes.cpp）。
// ============================================================================
#ifndef SYSY_STRUCTURED_IRGENIMPL_H
#define SYSY_STRUCTURED_IRGENIMPL_H

#include <cstdint>

#include "frontend/Type.h"
#include "structured/StructuredIR.h"

namespace sysy {
namespace sir {

// 【前置】t 是 Sema 的类型（可能为 nullptr：残缺树）。
// 【后置】返回 IR 类型（interned，可指针比较）。形状规则：
//   int → i32 · float → f32 · void → void
//   int[N]             → ptr[[N x i32]]          ← 局部/全局**对象**（Alloca/GlobalVar）
//   int[N][M]          → ptr[[N x [M x i32]]]
//   int[]  （形参，第 0 维未知） → ptr[i32]       ← ★ 数组形参**退化成指针**
//   int[][M]（形参）   → ptr[[M x i32]]
//   null → i32（保守占位：残缺树不让下游拿到 nullptr）
// 【为什么形参要退化】SysY 传数组就是传首元素地址（规范 §3 FuncFParam 2："the
//   parameter is a pointer to the first element"），所以形参在 IR 里**是指针**，
//   不是数组对象。否则 `a[i][j]` 的地址计算要为形参单开一条路径。
const Type* toIrType(const sysy::Type* t);

// 【后置】"指针当前所指的数组对象"被索引一次时的**元素步长**（元素个数），
//         即该对象去掉最外层之后剩余维度的元素总数。不可知 / 非数组 → 1。
int64_t strideElems(const Type* semTy);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_IRGENIMPL_H
