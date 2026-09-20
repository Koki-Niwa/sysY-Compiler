// ============================================================================
// SemaStmt.cpp —— 语句的语义检查（S03）
//
// 规范依据：`docs/sysy_lang.txt` §3 Stmt / §3 Block / §3 FuncDef / §3 LVal。
//
// 本文件负责：
//   * 块作用域的进出（块内遮蔽外层是**合法**的；同作用域重复定义才报错）
//   * `LVal = Exp` 的**顺序**：先算左值（拿到对象类型与可写性判定），
//     再把右值的期望类型定成左值类型 ⇒ SemaStmt.cpp + doAssignRhs
//   * `if`/`while` 的条件：float 条件必须被 `ToBool` 包住（**不是** FloatToInt！）
//   * `return` 的两条互补规则（规范 §3 FuncDef 1/2）
//
// ⚠️ `break` / `continue` 出现在循环外是**语法**约束，S02 的 Parser 已经用
//    `loopDepth_` 报过了（prompt §3.2 第 16 条），S03 **不要重复报**。
// ============================================================================
#include <string>
#include <string_view>
#include <utility>

#include "frontend/Sema.h"

namespace sysy {

namespace D = sema_diag;

// ============================================================================
// 条件位置：把 float 条件包成 `ToBool`
//
//   ★ 语义（prompt §3.3 的警告）：`ToBool` 是"值 ≠ 0"，**不是**"截断成 int"。
//     `(int)0.5 == 0`，所以 `if (0.5)` **必须**为真；IR 层是
//     `fcmp une x, 0.0`，不是 `fptosi`。
//   ★ 已经是 int 的位置**不插**（多余转换会让 IR 变脏，prompt §三明确要求）。
// ============================================================================
void Sema::doCondFix(Frame& f) {
  if (f.slot == nullptr) return;
  Expr* e = f.slot->get();
  if (e == nullptr || e->type == nullptr) return;
  if (isInt(e->type)) return;
  if (isFloat(e->type)) {
    const Type* to = typeContext().intType();
    std::unique_ptr<Cast> c(new Cast(e->loc, CastKind::ToBool, to, nullptr));
    c->type = to;
    c->operand = std::move(*f.slot);
    *f.slot = std::move(c);
    return;
  }
  if (isVoid(e->type)) {
    errorExpr(*e, D::kVoidValue, "void 函数调用不能作为条件（条件需要一个 int 值）");
    return;
  }
  errorExpr(*e, D::kType, "条件的类型必须是 int（实际是 " + typeText(e->type) + "）");
}

// ============================================================================
// 语句分派
// ============================================================================
void Sema::doStmt(Frame& f) {
  auto* s = static_cast<Stmt*>(f.node);
  if (s == nullptr) return;
  const std::string_view k = s->nodeKind();

  // ── 块：作用域进出 ────────────────────────────────────────────────────
  if (k == "BlockStmt") {
    auto* b = static_cast<BlockStmt*>(s);
    Frame pop;
    pop.kind = TK::PopScope;
    push(pop);             // 最后执行
    pushItems(b->items);   // 先执行：块内的声明与语句（按源码顺序）
    scopes_.push();        // ★ 立即压栈：块内声明的可见性从这一刻开始
    return;
  }

  // ── 赋值：左值 → （按左值类型决定右值期望类型）→ 右值 ────────────────
  if (k == "AssignStmt") {
    auto* a = static_cast<AssignStmt*>(s);
    if (a->lhs == nullptr) return;
    // 标记"这个 LVal 是赋值左侧"：S03 用它判 E-NOT-VAR / E-CONST-ASSIGN。
    // ⚠️ `lhs` 是 `unique_ptr<LVal>`（S02 的既有字段），而"插转换"只可能发生在
    //    `unique_ptr<Expr>` 槽位上；赋值左侧**永远不需要插转换**（它没有期望
    //    类型），所以这里传 slot = nullptr 是安全的，不必动 S02 的字段类型。
    a->lhs->isAssignTarget = true;
    Frame after;
    after.kind = TK::AssignRhs;
    after.node = s;
    push(after);                                       // 左值算完才执行
    pushExpr(*a->lhs, nullptr, nullptr, 0);
    return;
  }

  // ── 表达式语句 ────────────────────────────────────────────────────────
  if (k == "ExprStmt") {
    auto* e = static_cast<ExprStmt*>(s);
    if (e->expr == nullptr) return;                    // 空语句 `;`
    // kStmtLevel：允许 `(Call ... :void)` 作为整条表达式语句（§七 不变式 16）
    pushExpr(*e->expr, &e->expr, nullptr, kStmtLevel);
    return;
  }

  // ── if ────────────────────────────────────────────────────────────────
  if (k == "IfStmt") {
    auto* i = static_cast<IfStmt*>(s);
    if (i->elseS != nullptr) pushStmt(i->elseS.get());   // 执行顺序：cond → then → else
    if (i->thenS != nullptr) pushStmt(i->thenS.get());
    Frame cf;
    cf.kind = TK::CondFix;
    cf.slot = &i->cond;
    push(cf);
    if (i->cond != nullptr) pushExpr(*i->cond, &i->cond, nullptr, kCondCtx);
    return;
  }

  // ── while ─────────────────────────────────────────────────────────────
  if (k == "WhileStmt") {
    auto* w = static_cast<WhileStmt*>(s);
    if (w->body != nullptr) pushStmt(w->body.get());
    Frame cf;
    cf.kind = TK::CondFix;
    cf.slot = &w->cond;
    push(cf);
    if (w->cond != nullptr) pushExpr(*w->cond, &w->cond, nullptr, kCondCtx);
    return;
  }

  // ── return（规范 §3 FuncDef 1/2）──────────────────────────────────────
  if (k == "ReturnStmt") {
    auto* r = static_cast<ReturnStmt*>(s);
    if (r->value == nullptr) {
      if (!curRetIsVoid_) {
        error(r->loc, D::kRetMissing,
              "非 void 函数的 `return;` 必须带返回值（规范 §3 FuncDef 1）");
      }
      return;
    }
    if (curRetIsVoid_) {
      error(r->loc, D::kRetValue,
            "void 函数的 `return` 不能带值（规范 §3 FuncDef 2）");
      pushExpr(*r->value, &r->value, nullptr, 0);   // 仍然分析，保证转储完整
      return;
    }
    pushExpr(*r->value, &r->value, curRetType_, 0);
    return;
  }

  // break / continue：循环深度由 S02 的语法层判定，这里**不重复报**。
}

// ============================================================================
// 赋值的右值：期望类型 = 左值类型（prompt §3.3 决策点 #4）
// ============================================================================
void Sema::doAssignRhs(Frame& f) {
  auto* a = static_cast<AssignStmt*>(f.node);
  if (a == nullptr || a->lhs == nullptr || a->rhs == nullptr) return;
  const Type* lt = a->lhs->type;
  // 左值不是标量（例如整数组整体赋值 `a = ...`）时，LVal 的 post 已经报过
  // E-ARRAY-RANK；这里不再给期望类型，避免同一个位置报第二条（prompt §3.5：
  // "一条错误只报一次"）。
  pushExpr(*a->rhs, &a->rhs, isNumeric(lt) ? lt : nullptr, 0);
}

}  // namespace sysy
