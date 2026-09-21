// ============================================================================
// IRGenTypes.cpp —— Sema 类型 → 结构化 IR 类型的翻译（**唯一实现**）
//   ⚠️ 这里不做语义判断：类型是 Sema 填好的；本文件只负责"换个表示"。
//   为什么单独一个文件：把"类型怎么翻译"混进"怎么生成指令"里会让两件事各自
//   都更难核对（§C4 单一职责）；而且读回器/检查器也可能要用同一条规则。
// ============================================================================
#include "structured/IRGenImpl.h"

namespace sysy {
namespace sir {

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
