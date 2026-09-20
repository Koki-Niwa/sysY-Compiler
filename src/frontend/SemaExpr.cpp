// ============================================================================
// SemaExpr.cpp —— 表达式的语义检查与隐式转换插入（S03）
//
// 规范依据：`docs/sysy_lang.txt` §3 Exp and Cond / §3 LVal / §3 Implicit Type
// Conversions / §1 Overview（运算符优先级、"非零为真"、"关系与逻辑运算的结果
// 表示为 1/0"）。
//
// ── 六个决策点在哪（prompt §3.3）───────────────────────────────────────────
//   ① 二元算术 `+ - * /`  → Binary 分支：两侧提升到公共类型，int 侧插 IntToFloat
//   ② 二元 `%`            → Binary 分支：两侧**必须都是 int**，不做提升，
//                            否则 E-MOD-FLOAT（指令集里没有 frem）
//   ③ 关系 `< > <= >= == !=` → Binary 分支：两侧提升，**结果恒为 int**
//   ④ 赋值                → doAssignRhs（SemaStmt.cpp）
//   ⑤ 初始化 / 实参 / 返回值 → doExprPost 末尾的 applyExpected + Call 分支
//   ⑥ 布尔位置            → doCondFix（if/while）、Binary 的 `&&`/`||`、Unary 的 `!`
//
// ── ★ 不变式（§七 会机械检查它）─────────────────────────────────────────
//   Sema 之后：任何布尔位置上的表达式类型都是 int；任何赋值/实参/返回/算术
//   位置上，两侧类型都相同。ToBool 的语义是"值 ≠ 0"，**不是**截断成 int。
//
// ── 关于"一条错误只报一次"（prompt §3.5）────────────────────────────────
//   子表达式出错时一律给一个**占位类型**（int），这样父节点的检查不会因为
//   "类型未知"再报第二条 —— 同一个节点既报 E-TYPE 又报 E-ARGTYPE 是明令
//   禁止的噪声。
// ============================================================================
#include <string>
#include <string_view>
#include <utility>

#include "frontend/Sema.h"

namespace sysy {

namespace D = sema_diag;

// ============================================================================
// 把一个表达式就地包成转换节点
// ============================================================================
void Sema::wrapCast(std::unique_ptr<Expr>* slot, CastKind ck, const Type* target) {
  if (slot == nullptr || slot->get() == nullptr) return;
  Expr* inner = slot->get();
  std::unique_ptr<Cast> c(new Cast(inner->loc, ck, target, nullptr));
  c->type = target;
  c->operand = std::move(*slot);
  *slot = std::move(c);
}

// ============================================================================
// 进入表达式：决定要不要下钻子节点
// ============================================================================
void Sema::doExprEnter(Frame& f) {
  Expr* e = f.expr;
  if (e == nullptr) return;

  // 后序帧：无论哪种节点都要"算自己的类型 + 套期望类型"，所以统一先压它。
  Frame post = f;
  post.kind = TK::ExprPost;
  push(post);

  const std::string_view k = e->nodeKind();
  const uint8_t sub = static_cast<uint8_t>(f.flags & kCondCtx);   // 只下传条件上下文

  if (k == "Binary") {
    auto* b = static_cast<Binary*>(e);
    // 逆序压：先 rhs 后 lhs（栈是后进先出）。
    // ⚠️ 操作数的期望类型是 nullptr：两边的公共类型要**两个都算完**才知道，
    //    提升在 Binary 的后序里做（决策点 ①②③）。
    if (b->rhs != nullptr) pushExpr(*b->rhs, &b->rhs, nullptr, sub);
    if (b->lhs != nullptr) pushExpr(*b->lhs, &b->lhs, nullptr, sub);
    return;
  }
  if (k == "Unary") {
    auto* u = static_cast<Unary*>(e);
    // ★ 决策点 ⑥/规范 §3 Exp and Cond 1：
    //   "Exp ... does not include `!` among its unary operators, while the
    //    latter [Cond] may include it." ⇒ `int a = !b;` 非法。
    //   判定方法（prompt §3.2-13）：`!` 的整棵子树若位于某个 if/while 的
    //   条件之内则合法。子树内的 `!` 由 kCondCtx 继续下传覆盖
    //   （实测语料里有 `if (i0 == !i1 && ...)` 这种嵌在二元运算里的写法）。
    if (u->op == TokKind::Not && (f.flags & kCondCtx) == 0) {
      errorExpr(*e, D::kNotContext,
                "`!` 只能出现在 if/while 的条件里（规范 §3 Exp and Cond 1）");
    }
    if (u->operand != nullptr) pushExpr(*u->operand, &u->operand, nullptr, sub);
    return;
  }
  if (k == "LVal") {
    auto* lv = static_cast<LVal*>(e);
    // 下标表达式**不给期望类型**：规范要求"下标表达式的类型必须是 int"，
    // float 下标是**错误**而不是隐式转换（prompt §3.2-5 ⇒ E-INDEX-TYPE）。
    for (size_t i = lv->indices.size(); i-- > 0;) {
      if (lv->indices[i] != nullptr) pushExpr(*lv->indices[i], &lv->indices[i], nullptr, sub);
    }
    return;
  }
  if (k == "Call") {
    auto* c = static_cast<Call*>(e);
    // ★ 实参带 kArgPos：这是"允许子数组传递"的**唯一**位置（prompt §3.2-4）。
    //   实参的转换在 Call 的后序里做（数组实参的诊断编号是 E-ARGTYPE）。
    for (size_t i = c->args.size(); i-- > 0;) {
      if (c->args[i] != nullptr) {
        pushExpr(*c->args[i], &c->args[i], nullptr, static_cast<uint8_t>(sub | kArgPos));
      }
    }
    return;
  }
  // IntLit / FloatLit / Cast：叶子节点，后序帧负责算类型。
}

// ============================================================================
// 表达式的后序动作
// ============================================================================
void Sema::doExprPost(Frame& f) {
  Expr* e = f.expr;
  if (e == nullptr) return;
  const std::string_view k = e->nodeKind();
  TypeContext& t = typeContext();

  if (k == "IntLit" || k == "FloatLit") {
    // ★ 字面量的解析权在 ConstEval（prompt §四）：S02 只在语法层"顺手"算了
    //   一个辅助值（`parsed` 标志），`09` 这种非法八进制它只给 warning。
    //   语义阶段必须报错 —— 绝不能把它当成 9。
    if (k == "IntLit") {
      auto* lit = static_cast<IntLit*>(e);
      int32_t v = 0;
      std::string err;
      if (!ConstEvaluator::parseIntLit(*lit, v, err)) errorExpr(*e, D::kType, err);
      e->type = t.intType();
    } else {
      auto* lit = static_cast<FloatLit*>(e);
      float v = 0.0f;
      std::string err;
      if (!ConstEvaluator::parseFloatLit(*lit, v, err)) errorExpr(*e, D::kType, err);
      e->type = t.floatType();
    }
  } else if (k == "Unary") {
    auto* u = static_cast<Unary*>(e);
    const Type* ot = (u->operand != nullptr) ? u->operand->type : nullptr;
    if (u->op == TokKind::Not) {
      if (isFloat(ot)) {
        // 决策点 ⑥：`!` 的操作数是布尔位置 ⇒ 插 ToBool（值 ≠ 0），不是截断。
        wrapCast(&u->operand, CastKind::ToBool, t.intType());
      } else if (ot != nullptr && isVoid(ot)) {
        errorExpr(*e, D::kVoidValue, "void 函数调用不能作为 `!` 的操作数");
      } else if (ot != nullptr && !isInt(ot)) {
        errorExpr(*e, D::kType, "`!` 的操作数必须是标量数值（实际是 " + typeText(ot) + "）");
      }
      e->type = t.intType();     // 规范 §1：逻辑运算的结果表示为 1/0
    } else {                     // 一元 + / -
      if (ot == nullptr) {
        e->type = t.intType();
      } else if (isVoid(ot)) {
        errorExpr(*e, D::kVoidValue, "void 函数调用不能作为一元运算符的操作数");
        e->type = t.intType();
      } else if (!isNumeric(ot)) {
        errorExpr(*e, D::kType, "一元 +/- 的操作数必须是标量数值（实际是 " + typeText(ot) + "）");
        e->type = t.intType();
      } else {
        e->type = ot;            // 一元运算不改变类型
      }
    }
  } else if (k == "Binary") {
    auto* b = static_cast<Binary*>(e);
    const Type* lt = (b->lhs != nullptr) ? b->lhs->type : nullptr;
    const Type* rt = (b->rhs != nullptr) ? b->rhs->type : nullptr;
    const bool hasVoid = isVoid(lt) || isVoid(rt);
    const bool bothNum = isNumeric(lt) && isNumeric(rt);
    const Type* common = (isFloat(lt) || isFloat(rt)) ? t.floatType() : t.intType();

    switch (b->op) {
      case TokKind::Plus: case TokKind::Minus:
      case TokKind::Star: case TokKind::Slash:
        // 决策点 ①：int 参与 float 运算时提升
        if (hasVoid) {
          errorExpr(*e, D::kVoidValue, "void 函数调用不能出现在算术运算里");
          e->type = t.intType();
        } else if (!bothNum) {
          errorExpr(*e, D::kType, "算术运算的操作数必须是标量数值（实际是 " +
                                      typeText(lt) + " 与 " + typeText(rt) + "）");
          e->type = t.intType();
        } else {
          if (!sameType(lt, common)) wrapCast(&b->lhs, CastKind::IntToFloat, common);
          if (!sameType(rt, common)) wrapCast(&b->rhs, CastKind::IntToFloat, common);
          e->type = common;
        }
        break;
      case TokKind::Percent:
        // 决策点 ②：**不做** int→float 提升，直接报错（指令集里没有 frem）
        if (isFloat(lt) || isFloat(rt)) {
          errorExpr(*e, D::kModFloat,
                    "`%` 的两个操作数必须都是 int（SysY 的 `%` 只对整数有意义；"
                    "浮点取余 frem 不在我们的指令集里）");
          e->type = t.intType();
        } else if (hasVoid) {
          errorExpr(*e, D::kVoidValue, "void 函数调用不能出现在 `%` 里");
          e->type = t.intType();
        } else if (!isInt(lt) || !isInt(rt)) {
          errorExpr(*e, D::kType, "`%` 的操作数必须是 int");
          e->type = t.intType();
        } else {
          e->type = t.intType();
        }
        break;
      case TokKind::Less: case TokKind::Greater:
      case TokKind::LessEq: case TokKind::GreaterEq:
      case TokKind::EqEq: case TokKind::NotEq:
        // 决策点 ③：两侧提升到公共类型；结果**恒为 int**
        if (hasVoid) {
          errorExpr(*e, D::kVoidValue, "void 函数调用不能出现在关系运算里");
          e->type = t.intType();
        } else if (!bothNum) {
          errorExpr(*e, D::kType, "关系/相等运算的操作数必须是标量数值（实际是 " +
                                      typeText(lt) + " 与 " + typeText(rt) + "）");
          e->type = t.intType();
        } else {
          if (!sameType(lt, common)) wrapCast(&b->lhs, CastKind::IntToFloat, common);
          if (!sameType(rt, common)) wrapCast(&b->rhs, CastKind::IntToFloat, common);
          e->type = t.intType();
        }
        break;
      case TokKind::AmpAmp: case TokKind::PipePipe:
        // 决策点 ⑥：两个操作数都是布尔位置 ⇒ float 侧插 ToBool，int 侧**不插**
        if (hasVoid) {
          errorExpr(*e, D::kVoidValue, "void 函数调用不能出现在 `&&`/`||` 里");
        } else {
          if (isFloat(lt)) wrapCast(&b->lhs, CastKind::ToBool, t.intType());
          else if (lt != nullptr && !isInt(lt))
            errorExpr(*b->lhs, D::kType, "`&&`/`||` 的操作数必须是标量数值（实际是 " + typeText(lt) + "）");
          if (isFloat(rt)) wrapCast(&b->rhs, CastKind::ToBool, t.intType());
          else if (rt != nullptr && !isInt(rt))
            errorExpr(*b->rhs, D::kType, "`&&`/`||` 的操作数必须是标量数值（实际是 " + typeText(rt) + "）");
        }
        e->type = t.intType();
        break;
      default:
        errorExpr(*e, D::kType, "不支持的二元运算符");
        e->type = t.intType();
        break;
    }
  } else if (k == "LVal") {
    auto* lv = static_cast<LVal*>(e);
    const Symbol* sym = scopes_.lookup(lv->name);

    // ① 名字解析
    if (sym == nullptr) {
      errorExpr(*e, D::kUndef, "未定义的标识符 '" + lv->name + "'");
      lv->objType = t.intType();
      e->type = t.intType();
      applyExpected(f.slot, *e, f.expected, f.flags);
      return;
    }
    if (sym->kind == SymKind::Func) {
      // 变量与函数在**同一个名字空间**里查找（规范 §3 Conventions）：局部变量
      // 遮蔽函数名之后，这个名字就不再是函数了。
      if (lv->isAssignTarget) {
        errorExpr(*e, D::kNotVar, "赋值左侧不能是函数名 '" + lv->name + "'");
      } else {
        errorExpr(*e, D::kType, "函数名 '" + lv->name + "' 不能作为值使用");
      }
      lv->objType = sym->type;
      e->type = t.intType();
      applyExpected(f.slot, *e, f.expected, f.flags);
      return;
    }
    const Type* obj = sym->type;
    lv->objType = obj;

    // ② 下标表达式的类型必须是 int（规范 §3 Exp and Cond；float 下标报错，
    //    **不做**隐式转换 —— prompt §3.2-5）
    for (const std::unique_ptr<Expr>& ix : lv->indices) {
      if (ix == nullptr || ix->type == nullptr) continue;
      if (isInt(ix->type)) continue;
      if (isVoid(ix->type)) {
        errorExpr(*ix, D::kVoidValue, "void 函数调用不能作为下标");
      } else {
        errorExpr(*ix, D::kIndexType,
                  "下标表达式的类型必须是 int，实际是 " + typeText(ix->type) +
                      "（规范 §3 Exp and Cond；float 下标不做隐式转换）");
      }
    }

    // ③ 赋值左侧：不得是 const 对象（规范 §3 Exp and Cond 2）
    if (lv->isAssignTarget && sym->kind == SymKind::Const) {
      errorExpr(*e, D::kConstAssign, "不能给 const 对象 '" + lv->name + "' 赋值");
    }

    // ④ 下标个数与维数（规范 §3 LVal 2/3；prompt §3.2-4 的"唯一例外"）
    const int r = rank(obj);
    const int kk = static_cast<int>(lv->indices.size());
    if (r == 0 && kk > 0) {
      errorExpr(*e, D::kNotArray, "对标量 '" + lv->name + "' 取下标（规范 §3 LVal 3）");
      e->type = t.intType();
    } else if (kk > r) {
      errorExpr(*e, D::kArrayRank,
                "下标个数（" + std::to_string(kk) + "）超过了 '" + lv->name +
                    "' 的维数（" + std::to_string(r) + "）");
      e->type = t.intType();
    } else if (kk < r) {
      // ★ 唯一的例外：**实参位置**允许传子数组（规范 §3 FuncFParam 4；
      //   实证：语料 `performance/matmul1.sy:16` 的 `getarray(a[i])`）
      if ((f.flags & kArgPos) != 0) {
        e->type = dropDims(obj, kk);       // 值类型 = 去掉前 k 维后的数组
      } else {
        errorExpr(*e, D::kArrayRank,
                  "LVal 必须完全下标（规范 §3 LVal 2）：'" + lv->name + "' 是 " +
                      typeText(obj) + "，只给了 " + std::to_string(kk) + " 个下标");
        e->type = t.intType();
      }
    } else {
      e->type = dropDims(obj, kk);         // 完全下标 ⇒ 标量元素类型
    }
  } else if (k == "Call") {
    auto* c = static_cast<Call*>(e);
    const Symbol* sym = scopes_.lookup(c->callee);
    if (sym == nullptr) {
      // ★ 规范 §1 Overview："All variables/constants must be defined before use"，
      //   而 SysY **没有函数声明语法**（FuncDef 是唯一的函数形式）⇒ 调用一个
      //   "还没定义"的函数就是未定义标识符。**绝对不许**为了"让程序通过"而
      //   隐式前向声明 —— 那会把非法程序判成合法（prompt §3.1）。
      errorExpr(*e, D::kUndef,
                "未定义的函数 '" + c->callee +
                    "'（SysY 没有函数声明语法，调用前必须先定义）");
      e->type = t.intType();
      applyExpected(f.slot, *e, f.expected, f.flags);
      return;
    }
    if (sym->kind != SymKind::Func) {
      errorExpr(*e, D::kCallNonFunc, "'" + c->callee +
                                         "' 不是函数（该名字被变量/常量遮蔽）");
      e->type = t.intType();
      applyExpected(f.slot, *e, f.expected, f.flags);
      return;
    }
    const FuncSig* sig = sym->sig;
    if (sig == nullptr || sig->uncallable) {
      errorExpr(*e, D::kCallVarargs,
                "'" + c->callee + "' 是变参运行时函数，SysY 里没有字符串类型，不可调用");
      e->type = t.intType();
      applyExpected(f.slot, *e, f.expected, f.flags);
      return;
    }
    if (c->args.size() != sig->params.size()) {
      errorExpr(*e, D::kArgc,
                "实参个数与形参不符：'" + c->callee + "' 需要 " +
                    std::to_string(sig->params.size()) + " 个，实际 " +
                    std::to_string(c->args.size()) + " 个（规范 §3 Exp and Cond 3）");
    } else {
      for (size_t i = 0; i < c->args.size(); ++i) {
        Expr* a = c->args[i].get();
        if (a == nullptr || a->type == nullptr) continue;
        const Type* want = sig->params[i];
        const Type* got = a->type;
        if (isVoid(got)) {
          errorExpr(*a, D::kVoidValue, "void 函数调用不能作为实参");
          continue;
        }
        if (rank(want) > 0) {
          // ── 数组实参的匹配规则（prompt §3.2 第 8 条 / §3.5 E-ARGTYPE）──────
          //   * **元素类型必须相同**（这条不许放宽）；
          //   * 形参秩 r == 1（写成 `T[]`）：**任意秩的 T 数组都接受**。
          //     规范 §1 Overview 说数组参数按"传数组的起始地址"传递，
          //     §3 FuncFParam 4 的"一部分可以传"是**充分条件不是必要条件**：
          //     形参 `T[]` 只需要"一个 T 的首地址"，内部按 T 的步长平铺索引。
          //     ★ 实证：语料 `prelim_{arm,riscv}/performance/h-5-0{1,2,3}.sy` 与
          //       `h-8-0{1,2,3}.sy` 共 12 个文件写了
          //       `int A[1400][1400]; ... getarray(A); putarray(n*n, table);`
          //       （`h-5-01.in` 的第一个整数是 1960000 = 1400×1400 ⇒ 这个调用
          //        就是要整片填满二维数组）。
          //   * 形参秩 r >= 2：必须同秩，且第 1 维起逐维相同（第 0 维未知）。
          const bool shapeOk =
              (rank(want) == 1)
                  ? (rank(got) >= 1 && sameType(elementType(want), elementType(got)))
                  : (rank(got) == rank(want) &&
                     sameType(dropDims(got, 1), dropDims(want, 1)));
          if (!shapeOk) {
            errorExpr(*a, D::kArgType,
                      "实参类型不符：形参 '" + c->callee + "' 需要 " + typeText(want) +
                          "，实参是 " + typeText(got) + "（规范 §3 FuncFParam 4）");
          }
          continue;
        }
        if (!isNumeric(got)) {
          errorExpr(*a, D::kArgType, "实参类型不符：形参需要 " + typeText(want) +
                                         "，实参是 " + typeText(got));
          continue;
        }
        if (!sameType(got, want)) {
          // 规范 §1 Overview：int 与 float 之间支持隐式转换（两个方向都可以）
          wrapCast(&c->args[i],
                   isInt(got) ? CastKind::IntToFloat : CastKind::FloatToInt, want);
        }
      }
    }
    e->type = sig->ret;
  } else if (k == "Cast") {
    // Sema 自己插入的节点不会被再次访问；这里只是保险。
    auto* cd = static_cast<Cast*>(e);
    e->type = cd->target;
  } else {
    e->type = t.intType();     // 未知节点（语法错误残留）：给占位类型，保证转储完整
  }

  // ★ 决策点 ⑤ 的通用部分：把本节点的类型对齐到"期望类型"
  applyExpected(f.slot, *e, f.expected, f.flags);
}

}  // namespace sysy
