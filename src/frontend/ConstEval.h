// ============================================================================
// ConstEval —— 标量常量表达式求值（S03 交付物 #2）
//
// ── 边界（prompt §四）：**只做标量，只做 AddExp** ──────────────────────────
//   规范 `ConstExp -> AddExp`，而 AddExp 只含 `+ - * / %`、一元 `+ -`、
//   括号、字面量、LVal。**关系/相等/逻辑运算符不属于 ConstExp**，所以：
//     * `int a[1 < 2]` / `int a[x && 1]` / `int a[f(1)]` 一律不是常量表达式；
//     * `int g = (1 < 2);`（全局初始化器）同理 → E-CONST-INIT。
//   这不是"偷懒不做"：规范里这些运算符根本不在常量表达式的文法里，
//   把它们当常量求值会**接受非法程序**（而 prompt §六 明确禁止放宽规则）。
//
// ── ★ 字面量的解析权在本文件（prompt §四）────────────────────────────────
//   S02 只在语法层"顺手"算了个辅助值（`IntLit::parsed`），并明确写了
//   "语义上的唯一权威是常量求值"。所以：
//     * **进制判定只有这一份实现**（十进制/八进制/十六进制、十六进制浮点）；
//     * `09`（非法八进制）在这里**报错**，绝不当成 9；
//     * S04 的初始化器降级**必须**调用这里，不许再写第二份（双重真相
//       是这类项目最常见的隐性缺陷）。
//
// ── 语义决策（prompt §3.6，必须写进注释）────────────────────────────────
//   * `int` 是 32 位、二进制补码、**溢出回绕**（不产生 UB）⇒ 用 uint32_t 运算，
//     最后按位转回 int32_t（实现定义但 clang 上是回绕；更是我们**唯一**的
//     跨赛道一致性来源 —— 铁律 6）。
//   * `float` 是 IEEE-754 单精度。
//   * `x / 0`、`x % 0`、`INT_MIN / -1`：**归一化为 0**（守卫在 S05 插；
//     S03 不报错、不警告）。这里必须与 S05 完全一致，否则"常量求值"与
//     "运行时求值"会给出两个答案。
//   * float→int 越界是 UB（规范 §3 Implicit Type Conversions 1）：本文件里
//     常量折叠出现的越界按饱和处理（与 S05 的归一化一致），**不报错**。
// ============================================================================
#ifndef SYSY_FRONTEND_CONSTEVAL_H
#define SYSY_FRONTEND_CONSTEVAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/Type.h"

namespace sysy {

class DiagnosticEngine;

// ── 一个常量标量值 ────────────────────────────────────────────────────────
struct ConstValue {
  bool isFloat = false;
  int32_t i = 0;
  float f = 0.0f;

  ConstValue() = default;
  static ConstValue ofInt(int32_t v) { ConstValue c; c.isFloat = false; c.i = v; return c; }
  static ConstValue ofFloat(float v) { ConstValue c; c.isFloat = true;  c.f = v; return c; }
};

// ── 一个已定义的符号常量：类型 + **扁平行主序**的元素值 ────────────────────
//   标量：elems.size() == 1
//   数组：elems.size() == elementCount(type)（行主序展开）
//   "数组元素取值是**要求的**"（prompt §四）：`const int T[16]; ... T[3]`
//   必须能在编译期求出来，所以这里真的把每个元素算出来存下。
struct ConstObject {
  const Type* type = nullptr;
  std::vector<ConstValue> elems;
};

// ── 查表接口（由 Sema 实现；ConstEval 不依赖符号表的任何细节）─────────────
class ConstEnv {
 public:
  virtual ~ConstEnv() = default;
  // 【后置】返回该名字对应的符号常量；不存在 / 不是常量 / 不可见 → nullptr。
  virtual const ConstObject* findConst(const std::string& name) const = 0;
};

// ============================================================================
// ConstEvaluator
//
// 【为什么求值也是迭代的】`1+1+1+…`（3 万个 `+`）在语料里有真实对应
// （86_long_code2.sy 的树深 4007），而 `--emit=sema` 下不许崩（prompt §九）。
// 求值是**后序**遍历，递归实现会随树深增长 ⇒ 这里用显式工作栈。
// ============================================================================
class ConstEvaluator {
 public:
  ConstEvaluator(const ConstEnv& env, DiagnosticEngine& diag) : env_(env), diag_(diag) {}

  // 求一个标量常量表达式。
  // 【后置】成功 → out 有效并返回 true；失败 → 报**一条**诊断（编号 = code）
  //         并返回 false。不抛异常、不修改 AST。
  bool eval(const Expr& e, ConstValue& out, const char* code);

  // 求"维度长度"：必须是可编译期求出的**非负整数**。
  //   e == nullptr（写成 `[]`）→ 报错（形参首维之外的维度必须给长度）
  //   结果是浮点 / 负数 / 不是常量表达式 → 报错（编号 = code，默认 E-ARRAY-DIM）
  bool evalDim(const Expr* e, int64_t& out, const char* code);

  // ── 字面量解析（**权威实现**，S04 只能调用这里）────────────────────────
  // 【后置】成功 → out 有效、err 清空、返回 true。
  //         失败 → 返回 false 并把原因写进 err（**不报诊断**，由调用方决定编号）。
  static bool parseIntLit(const IntLit& lit, int32_t& out, std::string& err);
  static bool parseFloatLit(const FloatLit& lit, float& out, std::string& err);

 private:
  const ConstEnv& env_;
  DiagnosticEngine& diag_;
};

// ── 语义决策的可测试投影（S05 的 IRGen 必须与这里**逐位一致**）─────────────
int32_t normDivInt(int32_t a, int32_t b);   // x/0 → 0；INT_MIN/-1 → 0
int32_t normRemInt(int32_t a, int32_t b);   // x%0 → 0；INT_MIN%-1 → 0
int32_t satFptosi(float v);                 // NaN → 0；越界 → 饱和

}  // namespace sysy
#endif  // SYSY_FRONTEND_CONSTEVAL_H
