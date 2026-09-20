// ============================================================================
// InitLoweringDump.cpp —— `--emit=initplan` 的实现（格式契约见头文件）
// ============================================================================
#include "frontend/InitLoweringDump.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/SemaDump.h"
#include "frontend/Type.h"

namespace sysy {
namespace {

constexpr int kIndent = 2;   // 缩进 2 空格（与 ast/sema 同族）

void appendSpaces(std::string& out, int n) { out.append(static_cast<size_t>(n), ' '); }

// 【后置】把十进制无符号偏移写到 out（**没有**前导零、没有符号）。
void appendOffset(std::string& out, uint64_t v) { out += std::to_string(v); }

// ── 常量值的打印（§4.3）───────────────────────────────────────────────────
//   整型：`:int <十进制>`（负数带 `-`）
//   浮点：`:float <原文>` —— "原文"指 `FloatLit::text`（可能长成 `0x1.921fb6p+1`）。
//         ⚠️ 但 `InitPlan` 里存的是**求值之后**的 `ConstValue`，原文已经不在
//         手上了（`ConstValue` 是 12 字节的数值）。所以：
//           * 非零常量：按 `%a`（十六进制浮点）打印 —— 它是**无损**的
//             （C99 保证能往返），与 `putfloat` 的输出格式也一致；
//           * 零：打印 `0`（`-0.0` 打印 `-0x0p+0`，位模式不同不能混）。
//         §4.3 说"沿用 FloatLit::text"是**做不到**的（那个信息在降级时已经
//         丢了）；这是报告 §4 里交代的一处偏差。
void appendConst(std::string& out, const ConstValue& v) {
  if (v.isFloat) {
    out += ":float ";
    if (v.bits() == 0u) { out += "0"; return; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%a", static_cast<double>(v.f));
    out += buf;
  } else {
    out += ":int ";
    out += std::to_string(v.i);
  }
}

// 【后置】把一条 `(Global ...)` 追加到 out（自带换行）。
void appendGlobal(std::string& out, const InitPlan::GlobalData& g) {
  appendSpaces(out, kIndent);
  out += "(Global ";
  out += g.name;
  out += " :t ";
  appendTypeText(out, &g.type);
  if (g.allZero) {
    // ★ 整片为零 ⇒ 一个字的数据都不产生（§4.2 的强制项；语料里有 6 个
    //   文件各有 2×864 MB 的全局数组）。
    out += " :zero)\n";
    return;
  }
  out += " :data\n";
  for (size_t i = 0; i < g.nonzero.size(); ++i) {
    appendSpaces(out, 2 * kIndent);
    out += '(';
    appendOffset(out, g.nonzero[i].first);
    out += ' ';
    // `(偏移 值)`：值不带 `:int` / `:float` 记号（§4.3 的示例就是 `(0 1)`）。
    const ConstValue& v = g.nonzero[i].second;
    if (v.isFloat) {
      if (v.bits() == 0u) out += '0';
      else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%a", static_cast<double>(v.f));
        out += buf;
      }
    } else {
      out += std::to_string(v.i);
    }
    out += ')';
    if (i + 1 == g.nonzero.size()) out += ')';   // Global 自己的收尾括号
    out += '\n';
  }
}

// 【后置】把一条动作追加到 out（自带换行；`StoreExpr` 用**单行模式**折叠）。
void appendAction(std::string& out, const InitAction& a, int indent) {
  appendSpaces(out, indent);
  switch (a.kind) {
    case InitActionKind::Zero:
      out += "(Zero ";
      appendOffset(out, a.offset);
      out += ' ';
      appendOffset(out, a.bytes);
      out += ")\n";
      return;
    case InitActionKind::StoreConst:
      out += "(StoreConst ";
      appendOffset(out, a.offset);
      out += ' ';
      appendConst(out, a.value);
      out += ")\n";
      return;
    case InitActionKind::StoreExpr: {
      out += "(StoreExpr ";
      appendOffset(out, a.offset);
      out += ' ';
      // ★ `:t`：表达式自己的**值类型**，用与 `--emit=sema` 完全相同的记号
      //   （同一个函数，见 SemaDump.h）。`expr` 为空只是防御（不该发生）。
      if (a.expr != nullptr) {
        out += ':';
        appendTypeText(out, a.expr->type);
      } else {
        out += ":?";
      }
      out += ' ';
      if (a.expr != nullptr) {
        // ★ 复用 `--emit=sema` 的表达式打印器（**同一个**），单行折叠。
        //   它的输出里已经含了所有子节点、注解与右括号。
        out += printExprBody(*a.expr, /*singleLine=*/true);
      }
      out += ")\n";
      return;
    }
    case InitActionKind::MemcpyConst: {
      out += "(MemcpyConst ";
      appendOffset(out, a.offset);
      out += " :";
      // 元素类型：常量段里所有元素的类型一致（Sema 已经把它们对齐到元素类型）。
      out += (a.values.empty() ? "int" : (a.values[0].isFloat ? "float" : "int"));
      out += '\n';
      for (size_t i = 0; i < a.values.size(); ++i) {
        appendSpaces(out, indent + kIndent);
        const ConstValue& v = a.values[i];
        if (v.isFloat) {
          if (v.bits() == 0u) out += '0';
          else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%a", static_cast<double>(v.f));
            out += buf;
          }
        } else {
          out += std::to_string(v.i);
        }
        // 值一行一个（与 §4.3 的示例一致）；最后一个值与收尾括号同行。
        out += (i + 1 == a.values.size()) ? ")\n" : "\n";
      }
      if (a.values.empty()) out += ")\n";
      return;
    }
  }
}

}  // namespace

std::string printInitPlanDump(const InitPlan& plan) {
  std::string out;
  out.reserve(4096);
  out += "(InitPlan\n";

  // ── 头部：与 --emit=sema 完全相同的 13 行（**同一个实现**）──────────────
  std::string rt;
  appendRuntimeLibBlock(rt);          // 不含结尾换行
  out += rt;
  out += '\n';

  // ── 全局对象：一律先打印（它们只可能出现在顶层，源码顺序）──────────────
  for (const InitPlan::GlobalData& g : plan.globals) appendGlobal(out, g);

  // ── 局部对象：按函数分节；`Local` 用 `函数名/变量名` ─────────────────────
  std::string curFunc;
  bool openFunc = false;
  for (const InitPlan::LocalInit& l : plan.locals) {
    const size_t slash = l.name.find('/');
    const std::string fn = (slash == std::string::npos) ? std::string() : l.name.substr(0, slash);
    if (!openFunc || fn != curFunc) {
      curFunc = fn;
      openFunc = true;
      appendSpaces(out, kIndent);
      out += "(Func ";
      out += fn;
      out += '\n';
    }
    appendSpaces(out, 2 * kIndent);
    out += "(Local ";
    out += l.name;
    out += " :t ";
    appendTypeText(out, &l.type);
    out += " :actions\n";
    for (const InitAction& a : l.actions) appendAction(out, a, 3 * kIndent);
    // 收尾：每个 Local 自己一行 `)`（§4.3 的 `(MemcpyConst 8 :int 3 4 5))))`
    // 把 Local/Func/InitPlan 的括号都堆在最后一行 —— 那种"欠括号"写法是
    // 布局引擎的产物；这里显式逐行收，读起来更稳，也不违反任何一条规则。
    appendSpaces(out, 2 * kIndent);
    out += ")\n";
  }
  if (openFunc) {
    appendSpaces(out, kIndent);
    out += ")\n";
  }

  out += ")\n";
  return out;
}

}  // namespace sysy
