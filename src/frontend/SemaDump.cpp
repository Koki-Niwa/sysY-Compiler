// ============================================================================
// SemaDump.cpp —— `--emit=sema` 的注解策略 + `(RuntimeLib ...)` 头
//   格式契约与"与 prompt 的冲突"说明见 SemaDump.h 的文件头。
// ============================================================================
#include "frontend/SemaDump.h"

#include <string>
#include <string_view>

#include "frontend/AstSexpLayout.h"
#include "frontend/RuntimeLib.h"
#include "frontend/Type.h"

namespace sysy {
namespace {

// 类型记号一律带 `:` 前缀（`:int` / `:float` / `:void` / `:int[2]` / `:int[]`）。
// `:` 在 SysY 标识符里是非法字符 ⇒ 任何标识符都不可能被误认成类型记号，
// 于是"去注解还原"可以纯按记号前缀剥离，不需要理解语义。
void appendTypeToken(std::string& out, const Type* t) {
  out += " :";
  appendTypeText(out, t);
}

// ============================================================================
// `(RuntimeLib ...)` 这一整块（13 个运行时函数各占一行）。
//
// ★ 为什么抽成自由函数（S04）：`--emit=initplan` 的格式契约要求它的前 13 行
//   与 `--emit=sema` **逐字节相同**。若 initplan 自己再拼一遍这 13 行，那就有
//   两份真相：运行时签名表一变（比如以后加一个函数），两份立刻不一致，而
//   没有任何东西会拦住它。⇒ 只有这一份实现，两个 emit 都调用它。
//
// 【后置】out 里追加 `(RuntimeLib …)`（**不含**结尾换行；最后一个 `)` 是
//         RuntimeLib 的收尾括号，与 `--emit=sema` 的写法一致）。
// ============================================================================
void appendRuntimeLib(std::string& out, int indent) {
  const std::vector<RuntimeFunc>& funcs = runtimeFunctions();
  out.append(static_cast<size_t>(indent), ' ');
  out += "(RuntimeLib\n";
  for (size_t i = 0; i < funcs.size(); ++i) {
    const RuntimeFunc& f = funcs[i];
    out.append(static_cast<size_t>(indent + 2), ' ');
    out += "(RuntimeFunc ";
    out += f.name;
    appendTypeToken(out, f.sig->ret);
    if (f.sig->uncallable) {
      out += " :uncallable";
    } else {
      for (size_t k = 0; k < f.sig->params.size(); ++k) {
        out += " (Param ";
        // 运行时函数的形参在 SysY 源码里不可见，名字取 sylib.h 里的原名。
        out += (k == 0 && f.sig->params.size() > 1) ? 'n' : 'a';
        out += " :t ";
        appendTypeText(out, f.sig->params[k]);
        if (rank(f.sig->params[k]) > 0) out += " (Dim)";
        out += ')';
      }
    }
    out += ')';
    if (i + 1 == funcs.size()) out += ')';   // RuntimeLib 的收尾括号
    out += '\n';
  }
}

// ============================================================================
// SemaAnno —— `--emit=sema` 的注解策略
//   只**追加注解**，绝不参与换行 / 缩进 / 括号决策（那在 AstSexpLayout.h）。
// ============================================================================
struct SemaAnno {
  // 转储的第一行固定是 `(RuntimeLib)`（§五）；实现见上面的 appendRuntimeLib。
  static void preamble(std::string& out) { appendRuntimeLib(out, 0); }

  static void varDefHead(std::string& out, const VarDef& n) {
    out += " :t ";
    appendTypeText(out, n.semType);
  }

  static void paramHead(std::string& out, const Param& n) {
    out += " :t ";
    appendTypeText(out, n.semType);
  }

  // `(LVal <名字> :obj <对象类型> :<值类型>` —— 两个记号都紧跟名字，
  // 下标照旧另起一行缩进（那是 --emit=ast 本来就有的换行）。
  static void lvalHead(std::string& out, const LVal& n) {
    out += " :obj ";
    appendTypeText(out, n.objType);
    appendTypeToken(out, n.type);
  }

  // `:<值类型>`，插在节点头之后、第一个子节点之前（prompt §五(b)）。
  static void valueType(std::string& out, const Expr& n) { appendTypeToken(out, n.type); }

  // `(Cast :种类 :目标类型 <子节点>)` —— **只有两个记号**：
  // 它的值类型由种类唯一决定（IntToFloat→float，FloatToInt→int，ToBool→int），
  // 再写第三个记号就是冗余（prompt §五(d)）。
  static void castHead(std::string& out, const Cast& n) {
    out += " :";
    out += castKindName(n.kind);
    appendTypeToken(out, n.target);
  }
};

}  // namespace

std::string printSemaDump(const CompUnit& unit) {
  sexp::SexpLayout<SemaAnno> layout;
  return layout.run(unit);
}

// ============================================================================
// ★ S04 的加法：把"类型记号"与"子树正文"导出（`--emit=initplan` 复用）
//
//   `appendValueTypeAnnotation` 就是 `SemaAnno::valueType`（同一个函数体，
//   这里只是给它一个文件作用域的名字）—— 于是"记号怎么写"永远只有一份。
// ============================================================================
void appendValueTypeAnnotation(std::string& out, const Expr& e) { SemaAnno::valueType(out, e); }

void appendRuntimeLibBlock(std::string& out) { appendRuntimeLib(out, 2); }

std::string printExprBody(const Expr& e, bool singleLine) {
  sexp::SexpLayout<SemaAnno> layout;
  layout.setSingleLine(singleLine);
  return layout.runExpr(e);
}

}  // namespace sysy
