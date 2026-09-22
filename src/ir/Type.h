// ============================================================================
// ir/Type.h —— 平面层的**类型表示**（S06 交付物 #1 的前半）
//
// ── 为什么类型在 `ir/` 而不是 `structured/`（一次小的层级搬迁）────────────
//   这两层用的是**同一套类型记号**：`i32` / `f32` / `i64`（只用于地址算术）/
//   `void` / `ptr[T]` / `[N x T]`，文本拼法也一个字不差
//   （`docs/handoff/03-设计/平面IR与dump格式.md` §3）。
//   S05 把这些放在了 `structured/` 里，S06 的平面层若再用一份，
//   就会出现"同一个概念两份真理"——而铁律 7 的判据正是"改了以后编译器会不会
//   报错"：两份类型池之间**不会**报错，只会静默漂移。
//   ⇒ 类型下沉到 `ir/`（平面层是更底层），`structured/` 里加上
//     `using Type = flat::Type;` 等别名，**S05 的对外名字一个都没变**
//     （点名保留的 `sir::Type` / `sir::typePool()` / `sir::typeText()` 都还在，
//     见 StructuredIR.h 的兼容别名块）。
//
// ── 为什么是 typed pointer（`ptr[i32]`）而不是 LLVM 的 opaque `ptr` ────────
//   与 S05 的理由完全一致：子数组传参（`int[1400][1400]` 当 `int*`）之后，
//   指针的类型**不再是任何对象的类型**，只能显式携带。转换点因此只有一处
//   （`bitcast`），后端照结果类型发射即可。
//
// 【冻结】S06 结束后 `ir/` 的全部类接口**只增不改**（SESSION-PLAN §5）。
// ============================================================================
#ifndef SYSY_IR_TYPE_H
#define SYSY_IR_TYPE_H

#include <cstdint>
#include <string>
#include <vector>

namespace sysy {
namespace flat {

// 类型种类。**不发明新类型**（prompt §三.1）：与 `iset.txt` 和后端契约对齐。
//   Void —— 只做函数返回类型与 `ret void`
//   I32  —— SysY 的 `int`（也是全部**值**运算与比较结果的类型）
//   F32  —— SysY 的 `float`
//   I64  —— **只用于 getelementptr 的下标**（地址算术；没有 i64 的值运算）
//   Ptr  —— typed pointer，`elem` 是所指对象类型
//   Array—— `[len x elem]`，用于 alloca/global 的对象类型
enum class TypeKind : uint8_t { Void, I32, I64, F32, Ptr, Array };

struct Type {
  TypeKind kind = TypeKind::I32;
  const Type* elem = nullptr;   // Ptr 的元素类型 / Array 的元素类型
  int64_t len = 0;              // 仅 Array：元素个数（>= 0）

  bool isVoid() const { return kind == TypeKind::Void; }
  bool isPtr() const { return kind == TypeKind::Ptr; }
  bool isArray() const { return kind == TypeKind::Array; }
  bool isInt() const { return kind == TypeKind::I32; }
  bool isFloat() const { return kind == TypeKind::F32; }
  // 【后置】该类型是不是"标量值类型"（i32/f32/i64）——即可以参与算术的类型。
  bool isScalar() const { return kind == TypeKind::I32 || kind == TypeKind::I64 ||
                                 kind == TypeKind::F32; }
};

// 进程内唯一的类型池（interned：同构类型只有一个实例 ⇒ 指针相等 ⇔ 结构相等）。
// 生命期 = 进程（比任何 Module 长）。理由与 `sysy::typeContext()` 相同。
class TypePool {
 public:
  const Type* voidTy();
  const Type* i32();
  const Type* i64();
  const Type* f32();
  // 【前置】elem != nullptr。len >= 0。
  const Type* ptrTo(const Type* elem);
  const Type* arrayOf(const Type* elem, int64_t len);

 private:
  std::vector<Type*> pool_;
};

// 【后置】返回进程唯一的类型池。
TypePool& typePool();

// 【后置】把类型写成文本（`i32` / `ptr[i32]` / `[3 x [2 x i32]]`）。
void appendTypeText(std::string& out, const Type* t);
std::string typeText(const Type* t);

// 【前置】s 从 pos 起应当是一段类型文本（dump 里打印出来的那种）。
// 【后置】成功 → 返回类型并把 pos 推到类型末尾之后；失败 → 返回 nullptr，
//         pos 不动。**不抛异常**（读回畸形输入要能报错而不是崩，C5）。
const Type* parseTypeText(const std::string& s, size_t& pos);

// 【后置】该类型的**字节大小**（i32/f32 = 4；i64/ptr = 8；数组 = len × elem；
//         void/未知 = 0）。**溢出返回 -1**（不静默回绕）。
int64_t typeByteSize(const Type* t);

// 【后置】该类型的元素类型（数组 → 元素；非数组 → 自身）。
const Type* typeElem(const Type* t);

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_TYPE_H
