// ============================================================================
// Sema.cpp —— 任务机驱动 + 顶层（CompUnit / FuncDef / 作用域 / 名字表）
//
// 遍历架构与语义决策见 Sema.h 的文件头。本文件负责：
//   * 建立"运行时库层 → 全局层"的作用域栈
//   * 驱动显式工作栈的任务循环
//   * 顶层项（Decl / FuncDef）的先后顺序 —— **这就是"定义前使用"的判据**：
//     函数/全局变量在**被处理到的那一刻**才进名字表，于是"调用后文才定义的
//     函数"自然查不到 ⇒ E-UNDEF（规范 §1 Overview："All variables/constants
//     must be defined before use"；SysY **没有函数声明语法**，所以绝不允许
//     隐式前向声明 —— 那会把非法程序判成合法，prompt §3.1）
//   * `main` 的签名检查（规范 §3.1 CompUnit 1）
//   * 期望类型 → 隐式转换节点的插入（prompt §3.3）
//   * 作用域进出（保证错误路径上也不失衡）
// ============================================================================
#include "frontend/Sema.h"

#include <string>
#include <string_view>
#include <utility>

namespace sysy {

namespace D = sema_diag;

// ============================================================================
// 对外入口
// ============================================================================
void Sema::run(CompUnit& unit) {
  // ① 运行时库层：**每个函数里都可见、无需声明**（规范 §1 Overview）。
  //    放在栈底，于是"局部/全局变量遮蔽运行时函数名"自动成立
  //    （25_scope3.sy 实测：局部变量 `putch` 遮蔽同名函数）。
  for (const RuntimeFunc& rf : runtimeFunctions()) {
    Symbol s;
    s.kind = SymKind::Func;
    s.type = rf.sig->ret;
    s.sig = rf.sig;
    scopes_.declare(rf.name, s);
  }
  // ② 全局层
  scopes_.push();

  pushItems(unit.items);
  while (!stack_.empty()) {
    Frame f = stack_.back();
    stack_.pop_back();
    switch (f.kind) {
      case TK::Items:      doItems(f); break;
      case TK::FuncDef:    doFuncDef(f); break;
      case TK::ParamDecl:  doParamDecl(f); break;
      case TK::PopScope:   scopes_.pop(); break;
      case TK::Decl:       doDecl(f); break;
      case TK::VarDef:     doVarDef(f); break;
      case TK::InitRoot:   doInitRoot(f); break;
      case TK::InitGroup:  doInitGroup(f); break;
      case TK::Stmt:       doStmt(f); break;
      case TK::AssignRhs:  doAssignRhs(f); break;
      case TK::ExprEnter:  doExprEnter(f); break;
      case TK::ExprPost:   doExprPost(f); break;
      case TK::CondFix:    doCondFix(f); break;
      case TK::VarDefFinal: doVarDefFinal(f); break;
    }
  }

  // ③ 规范 §3.1 CompUnit 1：必须**恰好一个** `int main()`。
  //    "恰好一个"的另一半（重复定义）由顶层的 E-REDEF 负责。
  if (mainDef_ == nullptr) {
    error(unit.loc, D::kMain, "缺少 int main() 的定义（规范 §3.1 CompUnit 1）");
  } else if (!mainDef_->params.empty()) {
    error(mainDef_->loc, D::kMain, "main 的参数表必须为空（规范 §3.1 CompUnit 1）");
  } else if (mainDef_->isVoid || mainDef_->retType != BType::Int) {
    error(mainDef_->loc, D::kMain, "main 的返回类型必须是 int（规范 §3.1 CompUnit 1）");
  }

  // ★ S04：**故意不弹全局层**。理由：`Sema` 实现 `ConstEnv`，而
  //   `InitLowering` 在 `runInitPlan` 里要在 Sema 结束**之后**再求值一遍
  //   全局初始化器（它必须复用这里存的符号常量）。若在这里 `scopes_.pop()`，
  //   常量环境就空了，于是 `const int A = 3; const int B = A + 2;` 里的 B
  //   会静默变成 0 —— 实测就是这样（`--emit=initplan` 把 B 印成 `:zero`，
  //   而它应该是 5）。
  //   `scopes_` 一共只有两层（运行时库层 + 全局层），留着全局层不占什么内存，
  //   对 `run()` 的其它行为也没有影响（`run()` 每次自己压/弹函数与块作用域）。
}

const ConstObject* Sema::findConst(const std::string& name) const {
  const Symbol* s = scopes_.lookup(name);
  if (s == nullptr || s->kind != SymKind::Const) return nullptr;
  // cval == nullptr ⇒ 声明了但值还没求出来（自引用或求值失败）⇒ 当作"不是常量"
  return s->cval;
}

void runSema(CompUnit& unit, DiagnosticEngine& diag) {
  Sema sema(diag);
  sema.run(unit);
}

// ============================================================================
// 诊断（统一入口：一处执行 kMaxDiagErrors 上限，超限后继续遍历、仍退出 1）
// ============================================================================
void Sema::error(SourceLoc loc, const char* code, const std::string& msg) {
  if (diag_.errorCount() >= kMaxDiagErrors) return;
  diag_.report(DiagLevel::Error, loc, code, msg);
}

// ============================================================================
// 压帧助手
// ============================================================================
void Sema::pushItems(const std::vector<std::unique_ptr<Node>>& items) {
  Frame f;
  f.kind = TK::Items;
  f.items = &items;
  f.idx = 0;
  push(f);
}

void Sema::pushExpr(Expr& e, std::unique_ptr<Expr>* slot, const Type* expected, uint8_t flags) {
  Frame f;
  f.kind = TK::ExprEnter;
  f.expr = &e;
  f.slot = slot;
  f.expected = expected;
  f.flags = flags;
  push(f);
}

void Sema::pushStmt(Node* n) {
  Frame f;
  f.kind = TK::Stmt;
  f.node = n;
  push(f);
}

void Sema::pushDecl(Decl* d) {
  Frame f;
  f.kind = TK::Decl;
  f.node = d;
  f.idx = 0;
  push(f);
}

void Sema::pushVarDef(const Decl* d, VarDef* v) {
  Frame f;
  f.kind = TK::VarDef;
  f.node = v;
  f.decl = d;
  push(f);
}


// ============================================================================
// 一、顶层项：按**源码顺序**逐个处理 ⇒ "定义前使用"自动报 E-UNDEF
// ============================================================================
void Sema::doItems(Frame& f) {
  const std::vector<std::unique_ptr<Node>>& items = *f.items;
  if (f.idx >= items.size()) return;
  Node* item = items[f.idx].get();
  Frame next = f;
  next.idx = f.idx + 1;
  push(next);                    // 先把"继续下一个"压回去，保证一定前进
  if (item == nullptr) return;

  const std::string_view k = item->nodeKind();
  if (k == "Decl") {
    pushDecl(static_cast<Decl*>(item));
    return;
  }
  if (k == "FuncDef") {
    Frame g;
    g.kind = TK::FuncDef;
    g.node = item;
    push(g);
    return;
  }
  // Block 内的语句（BlockStmt 等）走同一条路（Items 同时服务 CompUnit 与 Block）
  if (k == "BlockStmt" || k == "AssignStmt" || k == "ExprStmt" || k == "IfStmt" ||
      k == "WhileStmt" || k == "BreakStmt" || k == "ContinueStmt" || k == "ReturnStmt") {
    pushStmt(item);
    return;
  }
  // 其它（语法错误的残留）：忽略，不报错 —— 语法阶段已经报过了
}

// ============================================================================
// 二、函数定义
// ============================================================================
void Sema::doFuncDef(Frame& f) {
  auto* fd = static_cast<FuncDef*>(f.node);
  if (fd == nullptr) return;

  TypeContext& t = typeContext();
  const Type* ret = fd->isVoid ? t.voidType()
                               : (fd->retType == BType::Int ? t.intType() : t.floatType());

  // ① 建签名（形参类型要逐维求值，所以必须在声明之前完成）
  sigs_.push_back(FuncSig{});
  FuncSig* sig = &sigs_.back();
  sig->ret = ret;
  sig->params.reserve(fd->params.size());
  for (Param& p : fd->params) {
    const Type* pt = buildArrayType(p.type.base, p.type.dims, /*isArrayParam=*/true);
    if (pt == nullptr) pt = t.intType();   // 维度非法：已报错，给个占位类型
    p.semType = pt;
    sig->params.push_back(pt);
  }

  // ② 声明函数（规范 §3.2 CompUnit 2：顶层不得重定义；prompt §3.4：用户函数
  //    不得与运行时库函数重名 —— 两者都报 E-REDEF）
  if (scopes_.declaredInCurrentScope(fd->name)) {
    error(fd->loc, D::kRedef, "顶层重复定义 '" + fd->name + "'（规范 §3.2 CompUnit 2）");
  } else if (isRuntimeFuncName(fd->name)) {
    error(fd->loc, D::kRedef, "函数 '" + fd->name + "' 与运行时库函数重名");
  } else {
    Symbol s;
    s.kind = SymKind::Func;
    s.type = ret;
    s.sig = sig;
    s.loc = fd->loc;
    scopes_.declare(fd->name, s);
  }
  if (fd->name == "main") mainDef_ = fd;

  if (fd->body == nullptr) return;   // 语法错误后的恢复形态：没有函数体

  // ③ 参数作用域 → 形参 → 函数体 → 弹作用域
  //    ⚠️ 参数与函数体是**两层**作用域：规范 §3 Block 2 说"块内可以重定义
  //       **块外**的同名声明"，而形参声明在函数头的括号里（块外）⇒
  //       `int f(int a) { int a; }` 按字面规则是合法的。把它判成错误会造成
  //       误报，而误报会同时废掉验收轨 A 与轨 C（prompt §六 的警告）。
  curRetType_ = ret;
  curRetIsVoid_ = fd->isVoid;

  Frame pop;
  pop.kind = TK::PopScope;
  push(pop);
  pushStmt(fd->body.get());
  for (size_t i = fd->params.size(); i-- > 0;) {
    Frame pf;
    pf.kind = TK::ParamDecl;
    pf.node = fd;
    pf.paramIndex = static_cast<uint32_t>(i);
    push(pf);
  }
  // ★ 形参的**维度表达式**也要作为表达式访问一遍：`int a[][5]` 里的 `5` 是一个
  //   真实的 `(IntLit 5)` 节点，转储里必须带值类型记号（否则出现 `:?`）。
  //   执行顺序：dims → 形参声明 → 函数体 ⇒ dims 最后压（最先执行）。
  for (Param& p : fd->params) {
    for (size_t i = p.type.dims.size(); i-- > 0;) {
      Dim& dm = p.type.dims[i];
      if (dm.expr != nullptr) pushExpr(*dm.expr, &dm.expr, nullptr, 0);
    }
  }
  scopes_.push();
}

void Sema::doParamDecl(Frame& f) {
  auto* fd = static_cast<FuncDef*>(f.node);
  if (fd == nullptr || f.paramIndex >= fd->params.size()) return;
  Param& p = fd->params[f.paramIndex];
  if (scopes_.declaredInCurrentScope(p.name)) {
    error(p.loc, D::kRedef, "形参 '" + p.name + "' 重复定义");
    return;
  }
  Symbol s;
  s.kind = SymKind::Var;
  s.type = p.semType;
  s.loc = p.loc;
  scopes_.declare(p.name, s);
}

// ============================================================================
// 三、期望类型 → 隐式转换（prompt §3.3 的 6 个决策点里有 4 个走这里）
//
//   规则：两侧类型不同且都是标量数值类型 ⇒ 插 Cast；
//         整型数组的元素位置遇到 float ⇒ E-TYPE（规范 §3 ConstDef 8 的不对称规则：
//         "the initializer list of an integer array may not contain floating-point
//          elements; however, the initializer list of a floating-point array may
//          contain integer constants"）；
//         其余（数组 / void / 形状不符）⇒ 报错，**不插**任何节点。
//
//   ⚠️ 这里**只**处理"标量 ↔ 标量"。实参位置的数组匹配（秩 + 尾维）在
//      SemaExpr.cpp 的 Call 分支里单独判，因为它的诊断编号是 E-ARGTYPE。
// ============================================================================
void Sema::applyExpected(std::unique_ptr<Expr>* slot, Expr& e, const Type* expected,
                         uint8_t flags) {
  if (slot == nullptr || expected == nullptr || e.type == nullptr) return;
  if (sameType(e.type, expected)) return;

  if (isVoid(e.type)) {
    errorExpr(e, D::kVoidValue, "void 函数调用不能用在需要值的位置");
    return;
  }
  if (isNumeric(e.type) && isNumeric(expected)) {
    if ((flags & kInitArrElem) != 0 && isInt(expected) && isFloat(e.type)) {
      errorExpr(e, D::kType,
                "整型数组的元素不能是浮点值（规范 §3 ConstDef 8）");
      return;
    }
    const CastKind ck = (isInt(e.type) && isFloat(expected)) ? CastKind::IntToFloat
                                                             : CastKind::FloatToInt;
    std::unique_ptr<Cast> c(new Cast(e.loc, ck, expected, nullptr));
    c->type = expected;
    c->operand = std::move(*slot);
    *slot = std::move(c);
    return;
  }
  errorExpr(e, D::kType, "类型不匹配：需要 " + typeText(expected) + "，实际是 " +
                             typeText(e.type));
}

}  // namespace sysy
