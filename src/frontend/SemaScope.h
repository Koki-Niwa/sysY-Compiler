// ============================================================================
// SemaScope —— 作用域与符号表（S03）
//
// 规范依据（`docs/sysy_lang.txt` §3 Conventions for identically named identifiers）：
//   * 全局变量与局部变量的作用域可以重叠，重叠区内**局部优先**；
//   * 同名局部变量的作用域**不得重叠**；
//   * **在 SysY 中，变量名可以与函数名相同**。
// 规范 §3.2 CompUnit 2：**顶层**的 Decl 与 FuncDef 不得重定义同一个标识符，
//   **即使类型不同**。
// 规范 §3 Block 1/2：块建立作用域；块内可以重定义外层同名声明，
//   作用域从**定义点**开始到块尾。
//
// ── 表示决定：**只有一张名字表**，变量与函数共用同一个名字空间 ────────────
//   语料里的 `final_riscv/h_functional/25_scope3.sy` 是这条决定的实证：
//   同一个函数里 `putch` 先是**函数**（前两个语句调用它），之后被
//   `int a = 1, putch = 0;` 变成**局部变量**。若把变量表与函数表分开，
//   这个用例要么查不到、要么查错 —— 所以"局部变量遮蔽同名函数"必须
//   由**同一张表**的遮蔽语义自然产生（TESTING-GUIDE §5 的 B 类用例：
//   C 编译器在这里报 `redefinition of 'putch'`，SysY 是合法的）。
//
// ── 作用域链是"由内向外的线性查找" ──────────────────────────────────────
//   没有全局作用域的特殊处理：全局作用域就是栈底那一层，运行时库的函数
//   被放在**更下面的一层**（`run()` 开始时先压一层放 13 个运行时函数）。
//   于是"局部变量遮蔽运行时函数"与"全局变量遮蔽运行时函数"自动成立，
//   不需要任何特判。
// ============================================================================
#ifndef SYSY_FRONTEND_SEMASCOPE_H
#define SYSY_FRONTEND_SEMASCOPE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "frontend/ConstEval.h"
#include "frontend/Type.h"
#include "support/SourceLoc.h"

namespace sysy {

enum class SymKind : uint8_t { Var, Const, Func };

struct Symbol {
  SymKind kind = SymKind::Var;
  const Type* type = nullptr;        // 对象类型（Var/Const）；Func 时是返回类型
  bool isConst = false;
  SourceLoc loc;
  const FuncSig* sig = nullptr;      // Func：函数签名
  const ConstObject* cval = nullptr; // Const：常量值（未求值完成为 nullptr）
};

// ============================================================================
// ScopeStack —— 作用域栈
//   底层层（scopes_[0]）是"运行时库层"，往上依次是全局层、块层……
//   isAtGlobal() 用于区分"全局声明"与"局部声明"（全局初始化器必须是常量表达式）。
// ============================================================================
class ScopeStack {
 public:
  ScopeStack() { scopes_.emplace_back(); }   // 运行时库层

  void push() { scopes_.emplace_back(); }
  void pop() {
    if (scopes_.size() > 1) scopes_.pop_back();
  }

  // 当前层是不是"用户可见的全局层"（即：栈里只有运行时库层 + 全局层）。
  bool atGlobalScope() const { return scopes_.size() <= 2; }

  // 当前作用域里**已经**声明过这个名字吗（用于同作用域重定义判定）。
  bool declaredInCurrentScope(const std::string& name) const {
    return scopes_.back().find(name) != scopes_.back().end();
  }

  // 【后置】成功把符号放进**当前**作用域并返回它（指针在作用域存活期间稳定：
  //   unordered_map 的 rehash 只失效迭代器，不失效元素的引用/指针）。
  //   当前作用域里已经有同名符号时返回 nullptr（调用方据此报 E-REDEF）。
  Symbol* declare(const std::string& name, const Symbol& s) {
    auto& m = scopes_.back();
    if (m.find(name) != m.end()) return nullptr;
    return &m.emplace(name, s).first->second;
  }

  // 【后置】只在**当前**作用域里找；找不到返回 nullptr。
  Symbol* findInCurrentScope(const std::string& name) {
    auto it = scopes_.back().find(name);
    return it == scopes_.back().end() ? nullptr : &it->second;
  }

  // 【后置】由内向外查找第一个匹配；找不到返回 nullptr。
  const Symbol* lookup(const std::string& name) const {
    for (size_t i = scopes_.size(); i-- > 0;) {
      const auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) return &it->second;
    }
    return nullptr;
  }

 private:
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};

}  // namespace sysy
#endif  // SYSY_FRONTEND_SEMASCOPE_H
