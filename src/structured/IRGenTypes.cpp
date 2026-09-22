// ============================================================================
// IRGenTypes.cpp —— Sema 类型 → 结构化 IR 类型的翻译（**唯一实现**）
//   ⚠️ 这里不做语义判断：类型是 Sema 填好的；本文件只负责"换个表示"。
//   为什么单独一个文件：把"类型怎么翻译"混进"怎么生成指令"里会让两件事各自
//   都更难核对（§C4 单一职责）；而且读回器/检查器也可能要用同一条规则。
// ============================================================================
#include "structured/IRGenImpl.h"

namespace sysy {
namespace sir {

// 【后置】`t` 作为**内存对象**的类型（`alloca`/全局对象要用的那个）。
//   ★★ 为什么不能直接用 `toIrType` ★★
//     `toIrType` 返回的是**值**的类型：对数组它给的是"指向数组的指针"
//     （见下面的 Array 分支）。`alloca` 要的却是"被分配的那个对象"的类型 ——
//     直接用它就会把 `int[2][1][3]` 分配成
//     `ptr[[2 x ptr[[1 x ptr[[3 x i32]]]]]]`（**指针的数组**，每层都错），
//     于是 `c[1][0][1]` 读出来是 0（实测：`.work/md2.sy` 平面/结构化都算 0，
//     而 gcc 是 81）。标量情形两者相同（`int` → `i32`，`alloca` 自己包一层）。
const Type* toIrObjType(const sysy::Type* t) {
  if (t == nullptr) return typePool().i32();
  switch (t->kind) {
    case sysy::TypeKind::Void:  return typePool().voidTy();
    case sysy::TypeKind::Int:   return typePool().i32();
    case sysy::TypeKind::Float: return typePool().f32();
    case sysy::TypeKind::Array: {
      // ⚠️ 必须**递归地用本函数**把元素类型也建成"对象类型"：
      //   调 `toIrType(t->elem)` 会在内层又包一层指针（`int[2][1][3]`
      //   变成 `[2 x ptr[[1 x ptr[[3 x i32]]]]]` —— 外层剥对了、内层还是错的）。
      const Type* elem = toIrObjType(t->elem);
      if (elem == nullptr || t->len == kUnknownDim) return elem;   // 形参退化
      return typePool().arrayOf(elem, t->len);
    }
  }
  return typePool().i32();
}

const Type* toIrType(const sysy::Type* t) {
  if (t == nullptr) return typePool().i32();
  switch (t->kind) {
    case sysy::TypeKind::Void:  return typePool().voidTy();
    case sysy::TypeKind::Int:   return typePool().i32();
    case sysy::TypeKind::Float: return typePool().f32();
    case sysy::TypeKind::Array: {
      // 形参第一维写成 `[]`（kUnknownDim）⇒ **退化**：`ptr[元素类型]`。
      // 这正是"数组形参就是指针"（规范 §3 FuncFParam 2）在类型上的落点：
      //   `int a[]`    → ptr[i32]
      //   `int a[][5]` → ptr[[5 x i32]]
      if (t->len == kUnknownDim) return typePool().ptrTo(toIrType(t->elem));
      const Type* elem = toIrType(t->elem);
      return typePool().ptrTo(typePool().arrayOf(elem, t->len));
    }
  }
  return typePool().i32();
}

int64_t strideElems(const sysy::Type* semTy) {
  if (semTy == nullptr || !semTy->isArray()) return 1;
  const int64_t n = elementCount(semTy);
  return n < 0 ? 1 : n;   // 含未知维 ⇒ 不可知，保守取 1（Sema 已报 E-ARRAY-DIM）
}

}  // namespace sir
}  // namespace sysy
