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
// SemaAnno —— `--emit=sema` 的注解策略
//   只**追加注解**，绝不参与换行 / 缩进 / 括号决策（那在 AstSexpLayout.h）。
// ============================================================================
struct SemaAnno {
  // 转储的第一行固定是 `(RuntimeLib)`（§五）：13 个运行时函数各占一行，
  // 形参用与 `(Param ...)` 相同的拼写，`putf` 标成 `:uncallable`。
  static void preamble(std::string& out) {
    out += "(RuntimeLib\n";
    const std::vector<RuntimeFunc>& funcs = runtimeFunctions();
    for (size_t i = 0; i < funcs.size(); ++i) {
      const RuntimeFunc& f = funcs[i];
      out += "  (RuntimeFunc ";
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

}  // namespace sysy
