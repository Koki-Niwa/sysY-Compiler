// ============================================================================
// Type —— SysY 类型表示（S03 交付物 #1）
//
// 规范依据：`docs/sysy_lang.txt` §1 Overview
//   * `int` 是 32 位有符号整数（二进制补码、溢出回绕，见 §3.6 的语义决策）
//   * `float` 是 IEEE-754 单精度
//   * 数组：元素类型为 int/float 的多维数组，**行主序**（row-major）
//   * `void` 只作返回类型
//   * **没有**指针 / 结构体 / 字符串类型（运行时的 `char*` 只在名字表里存在）
//
// ── 本文件只做"表示与比较"，不含任何语义逻辑 ──────────────────────────────
//   类型检查（谁能不能赋给谁、能不能隐式转换）属于 Sema；常量求值属于 ConstEval。
//   这里只回答"这个类型是什么形状"。
//
// ── 表示决定：**数组类型是"元素类型 + 长度"的递归结构**，不是"维度列表" ──
//   `int[2][3]` ≡ Array(elem = Array(elem = Int, len = 3), len = 2)
//   理由（与规范一致）：规范 §1 说形参"only the first dimension may omit its
//   length"，即 `int a[][5]` 的**元素类型**是 `int[5]`（一个完整的数组类型），
//   而"第 0 维未知"只影响最外层。用递归结构表达时：
//     * `a[1]` 的类型 = elem 类型 = `int[5]` —— 与 §3 FuncFParam 4 的
//       "a[1] is a one-dimensional array with three elements" 直接对应；
//     * `int[]` 与 `int[][5]` 分别是"第 0 维未知的一维/二维数组"，不会混。
//   若改用"维度列表 + 基类型"，每个使用点都要自己算"去掉前 k 维之后是什么"，
//   而那个算式在类型比较、子数组传参、LVal 值类型三处都要重写一遍。
//
// ── 长度用 int64_t，未知维用 -1 ──────────────────────────────────────────
//   维度长度必须"能在编译期求出非负整数"（规范 §3 ConstDef 3），但求值在
//   Sema 里做；这里只负责表示，"未知"必须与"0"区分开（`int[]` vs `int[0]`，
//   后者本身就是非法的、由 Sema 报 E-ARRAY-DIM）。
//
// ── 相等性用**指针相等**（interned）──────────────────────────────────────
//   TypeContext 保证同构类型只有一个实例，于是 `sameType` 退化成一次指针比较，
//   而"类型"在 AST 节点上可以直接存 `const Type*`（无拷贝、无所有权问题）。
//   这也是 SemaDump 能把类型注解直接挂在节点上的前提。
// ============================================================================
#ifndef SYSY_FRONTEND_TYPE_H
#define SYSY_FRONTEND_TYPE_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sysy {

enum class TypeKind : uint8_t { Void, Int, Float, Array };

// 数组维度的"未知长度"（形参第一维写成 `[]`）。
inline constexpr int64_t kUnknownDim = -1;

struct Type {
  TypeKind kind = TypeKind::Int;
  const Type* elem = nullptr;   // 仅 Array 有意义：元素类型
  int64_t len = 0;              // 仅 Array 有意义：>= 0 已知；-1 = 未知

  bool isArray() const { return kind == TypeKind::Array; }
  const char* nodeKind() const { return "Type"; }
};

// ── 隐式转换的种类（规范 §3 Implicit Type Conversions；prompt §3.3）────────
// 只有三种，且**必须物化成 AST 节点**，让 IRGen 永远不需要"猜"类型。
enum class CastKind : uint8_t {
  IntToFloat,   // int → float（规范 §3 Implicit Type Conversions 2：值不变）
  FloatToInt,   // float → int（规范 1：丢弃小数部分；越界 UB，归一化在 S05）
  ToBool,       // float → int 的"值 ≠ 0"判定（**不是**截断！见下）
};

// 转储里的拼写（`(Cast :IntToFloat :float <子> :float)`）。
inline const char* castKindName(CastKind k) {
  switch (k) {
    case CastKind::IntToFloat: return "IntToFloat";
    case CastKind::FloatToInt: return "FloatToInt";
    case CastKind::ToBool:     return "ToBool";
  }
  return "?";
}

// 函数签名（用户函数与运行时库函数共用）。
//   ret        返回类型；`void` 用 TypeContext::voidType()
//   params     形参类型（数组形参的第 0 维是 kUnknownDim 的 Array）
//   uncallable 变参 / 含 SysY 不支持的参数类型（`putf`）⇒ 调用一律报错
struct FuncSig {
  const Type* ret = nullptr;
  std::vector<const Type*> params;
  bool uncallable = false;
};

// ============================================================================
// TypeContext —— 类型工厂 + interning（保证指针相等 ⇔ 结构相等）
// ============================================================================
class TypeContext {
 public:
  TypeContext() = default;
  TypeContext(const TypeContext&) = delete;
  TypeContext& operator=(const TypeContext&) = delete;

  const Type* voidType() { return &void_; }
  const Type* intType() { return &int_; }
  const Type* floatType() { return &float_; }

  // 【前置】elem != nullptr。len >= 0 或 kUnknownDim。
  // 【后置】返回唯一实例；同样的 (elem, len) 永远返回同一个指针。
  //
  // ⚠️ 去重表的键是 **(elem, len)**，查表时比的必须是 **elem** 而不是已经建好的
  //    数组类型 —— S03 实测踩过一次：把 `it.first == elem` 写成"比数组类型指针"
  //    会让去重永远不命中，于是 `int[59]` 被造出很多份、`sameType` 的指针相等
  //    全线失效（表现为"形参 int[][59] 与实参 int[53][59] 不匹配"这种
  //    自相矛盾的诊断）。`sameType` 是纯指针比较，本函数是它唯一的前提。
  const Type* arrayOf(const Type* elem, int64_t len) {
    for (const ArrayKey& k : arrays_) {
      if (k.elem == elem && k.len == len) return k.result;
    }
    pool_.push_back(std::unique_ptr<Type>(new Type{TypeKind::Array, elem, len}));
    const Type* t = pool_.back().get();
    arrays_.push_back(ArrayKey{elem, len, t});
    return t;
  }

 private:
  struct ArrayKey {
    const Type* elem;
    int64_t len;
    const Type* result;
  };
  Type void_{TypeKind::Void, nullptr, 0};
  Type int_{TypeKind::Int, nullptr, 0};
  Type float_{TypeKind::Float, nullptr, 0};
  std::vector<std::unique_ptr<Type>> pool_;   // 数组类型
  std::vector<ArrayKey> arrays_;              // 去重表（键 = elem + len）
};

// ============================================================================
// 类型性质（纯查询，无副作用）
// ============================================================================

// 指针相等即结构相等（interned）。nullptr 与 nullptr 也算相等。
inline bool sameType(const Type* a, const Type* b) { return a == b; }

inline bool isVoid(const Type* t) { return t != nullptr && t->kind == TypeKind::Void; }
inline bool isInt(const Type* t) { return t != nullptr && t->kind == TypeKind::Int; }
inline bool isFloat(const Type* t) { return t != nullptr && t->kind == TypeKind::Float; }

// int / float（可以参与算术与隐式转换）
inline bool isNumeric(const Type* t) { return isInt(t) || isFloat(t); }
// 标量：int / float（**不含** void —— void 不是值类型）
inline bool isScalarValue(const Type* t) { return isNumeric(t); }

// 维数：标量 0；数组 = 最外层 + 元素维数。
inline int rank(const Type* t) {
  int r = 0;
  while (t != nullptr && t->kind == TypeKind::Array) { ++r; t = t->elem; }
  return r;
}

// 最内层元素类型（标量返回自身；void 返回 nullptr）。
inline const Type* elementType(const Type* t) {
  while (t != nullptr && t->kind == TypeKind::Array) t = t->elem;
  return (t == nullptr || t->kind == TypeKind::Void) ? nullptr : t;
}

// 去掉前 k 维。k >= rank 时返回最内层元素类型（标量）。
inline const Type* dropDims(const Type* t, int k) {
  while (k > 0 && t != nullptr && t->kind == TypeKind::Array) { t = t->elem; --k; }
  return t;
}

// 按行主序展开的元素总数。任何一维未知（-1）时返回 -1（"不可知"）。
inline int64_t elementCount(const Type* t) {
  int64_t n = 1;
  while (t != nullptr && t->kind == TypeKind::Array) {
    if (t->len < 0) return -1;
    n *= t->len;
    t = t->elem;
  }
  return n;
}

inline const Type* arrayElem(const Type* t) {
  return (t != nullptr && t->kind == TypeKind::Array) ? t->elem : nullptr;
}
inline int64_t arrayLen(const Type* t) {
  return (t != nullptr && t->kind == TypeKind::Array) ? t->len : 0;
}

// 后缀拼写（唯一的对外拼写，SemaDump 与诊断共用 —— **不许再写第二份**）：
//   int / float / void / int[2] / float[2][3] / int[]
inline void appendTypeText(std::string& out, const Type* t) {
  if (t == nullptr) { out += "?"; return; }
  const Type* base = t;
  int dims = 0;
  while (base != nullptr && base->kind == TypeKind::Array) { base = base->elem; ++dims; }
  switch (base == nullptr ? TypeKind::Void : base->kind) {
    case TypeKind::Void:  out += "void";  break;
    case TypeKind::Int:   out += "int";   break;
    case TypeKind::Float: out += "float"; break;
    case TypeKind::Array: break;   // 不可达（循环已走到最内层）
  }
  for (int i = 0; i < dims; ++i) {
    const Type* d = dropDims(t, i);
    out += '[';
    if (d->len >= 0) out += std::to_string(d->len);
    out += ']';
  }
}

inline std::string typeText(const Type* t) {
  std::string s;
  appendTypeText(s, t);
  return s;
}

// ★ 进程内唯一的类型工厂。
//   为什么是全局的而不是 Sema 的成员：Sema 结束后 AST 上仍然挂着 `const Type*`
//   （`--emit=sema` 在 Sema 返回之后才打印），若类型池随 Sema 一起析构，
//   那些指针立刻悬垂。类型池是"无状态的字面量常量表"，做成进程级单例即可，
//   生命期覆盖整个编译过程。一个进程只编译一个文件（比赛调用方式），
//   所以不存在跨文件污染。
inline TypeContext& typeContext() {
  static TypeContext ctx;
  return ctx;
}

}  // namespace sysy
#endif  // SYSY_FRONTEND_TYPE_H
