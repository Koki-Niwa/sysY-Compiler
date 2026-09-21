// ============================================================================
// IRGenStmt.cpp —— 结构化 IR 生成：**语句 + Region + 初始化降级**
//   同一个 `Gen` 类的另一半（见 IRGenInternal.h）；表达式与 Op 工厂在 IRGen.cpp。
//
// ── 一个关键的结构决定：**Region 的终结 Op 不许泄漏到外层** ────────────────
//   `WhileOp`/`IfOp` 的条件 Region 以 `YieldOp <bool>` 收尾（I4）。若按"先建
//   Region、再建 Op"的顺序写，那个 Yield 会被 push 进**当前** Region（外层），
//   外层就多出一个终结 Op ⇒ I4 被破坏。所以本文件里：
//     * 所有"生成到某个 Region"的辅助函数都**在内部保存/恢复 `cur_`**；
//     * 调用方一律 **Op 建好之后** 才 `emit(op)`。
// ============================================================================
#include "structured/IRGenInternal.h"

#include <cstring>

namespace sysy {
namespace sir {

// ============================================================================
// Region 生成（**终结 Op 由这里负责，绝不落到外层**）
// ============================================================================
Region* Gen::makeBodyRegion(const Stmt* body, SourceLoc loc, int depth) {
  Region* r = mkRegion();
  Region* saved = cur_;
  cur_ = r;
  const size_t mark = syms_.size();
  genStmt(body, depth + 1);
  syms_.resize(mark);
  if (r->empty() || !isTerminator(r->back()->kind)) {
    // 体正常结束 ⇒ 回到条件/续行（`YieldOp` = continue；I4 要求"恰好一个终结 Op"）
    terminator(OpKind::Yield, {}, loc);
  }
  cur_ = saved;
  return r;
}

// 【后置】在 dst 里算 cond，并以 `YieldOp <bool>` 收尾；**返回被 yield 的值**。
//   ⚠️ 不能写成"先建 Region、再从 `region->back()` 把操作数掏出来"：
//     `makeBodyRegion` 与嵌套生成的顺序会让"当时的 back()"不是那个 Yield
//     （实测：`If` 的条件操作数变成空 ⇒ dump 里印成 `%?`，而 `%?` 不是合法
//      操作数 ⇒ 读回器无法还原）。**显式把值传出来**是唯一稳的写法。
Value Gen::genCondValue(const Expr* cond, Region* dst, int depth) {
  Region* saved = cur_;
  cur_ = dst;
  Value v = genExpr(cond, depth + 1);
  if (v == nullptr) v = cInt(0, cond != nullptr ? cond->loc : SourceLoc());
  terminator(OpKind::Yield, {v}, cond != nullptr ? cond->loc : SourceLoc());
  cur_ = saved;
  return v;
}

// ============================================================================
// 语句
// ============================================================================
void Gen::genStmt(const Node* n, int depth) {
  if (n == nullptr) return;
  if (!depthOk(depth, n->loc)) return;
  const std::string_view k = n->nodeKind();

  if (k == "BlockStmt") {
    // SysY 的块**不是**控制流结构：它的语句平铺进当前 Region，只有作用域。
    // ★ 一旦某条语句**无条件终止**（`return`/`break`/`continue`/两分支都终止的
    //   `if`），它**之后**的语句是不可达代码 —— **不生成**（I4 要求终结 Op 是
    //   Region 的最后一行；生成死代码会让它出现在中间）。
    const auto* b = static_cast<const BlockStmt*>(n);
    const size_t mark = syms_.size();
    for (const auto& it : b->items) {
      if (it == nullptr) continue;
      genStmt(it.get(), depth + 1);
      if (terminates(it.get())) break;
    }
    syms_.resize(mark);
    return;
  }
  if (k == "Decl") {
    genDecl(*static_cast<const Decl*>(n), depth);
    return;
  }
  if (k == "AssignStmt") {
    const auto* a = static_cast<const AssignStmt*>(n);
    Value rhs = genExpr(a->rhs.get(), depth);
    Value lhs = genLValAddr(*a->lhs, depth);
    rhs = orZero(rhs, a->lhs->type, a->loc);
    const sysy::Type* vt = a->lhs->type;
    if (vt != nullptr && !vt->isArray()) store(toIrType(vt), rhs, lhs, a->loc);
    return;
  }
  if (k == "ExprStmt") {
    const auto* s = static_cast<const ExprStmt*>(n);
    if (s->expr != nullptr) (void)genExpr(s->expr.get(), depth);
    return;
  }
  if (k == "IfStmt") {
    const auto* s = static_cast<const IfStmt*>(n);
    // `IfOp` 的条件是**操作数**（不是 Region）⇒ 条件指令直接发在当前 Region 里，
    // 它的结果就是 If 的操作数。**不要**为它建一个 Region：那个 Region 不会被
    // 任何 Op 引用（实测症状：条件印成 `%?`，因为结果挂在一个游离的 Region 上）。
    const Value cval = genExpr(s->cond.get(), depth);
    Region* thenR = makeBodyRegion(s->thenS.get(), s->loc, depth);
    Region* elseR = mkRegion();
    {
      Region* saved = cur_;
      cur_ = elseR;
      const size_t mark = syms_.size();
      if (s->elseS != nullptr) genStmt(s->elseS.get(), depth + 1);
      syms_.resize(mark);
      if (elseR->empty() || !isTerminator(elseR->back()->kind)) {
        terminator(OpKind::Yield, {}, s->loc);
      }
      cur_ = saved;
    }
    // ★ I4：两个 Region 都必须有终结 Op（无 else 时也发 `yield`）
    Op* op = mk(OpKind::If, s->loc);
    op->addOperand(cval);
    op->addRegion(thenR);
    op->addRegion(elseR);
    emit(op);
    return;
  }
  if (k == "WhileStmt") {
    // ★ S05 **不产 ForOp**（归一化是 S05b，prompt §十一.1）⇒ 一律 WhileOp。
    const auto* s = static_cast<const WhileStmt*>(n);
    // `WhileOp` **有**条件 Region（prompt §4.1：While = 条件 Region + 体 Region）
    Region* condR = mkRegion();
    (void)genCondValue(s->cond.get(), condR, depth);
    Region* bodyR = makeBodyRegion(s->body.get(), s->loc, depth);
    Op* op = mk(OpKind::While, s->loc);
    op->addRegion(condR);
    op->addRegion(bodyR);
    emit(op);
    return;
  }
  if (k == "BreakStmt") {
    terminator(OpKind::Break, {}, n->loc);
    return;
  }
  if (k == "ContinueStmt") {
    // ★ prompt §四.1：S05 里 `BreakOp` **也承担 `continue`** —— 结构化层还没有
    //   循环体的归一化（S05b 做"continue 消解"）。`BreakOp` 的语义是"当前循环体
    //   Region 到此为止"，具体是 continue 还是 break 由 S05b 按形状判定。
    terminator(OpKind::Break, {}, n->loc);
    return;
  }
  if (k == "ReturnStmt") {
    // ★ `ReturnOp` 的操作数个数必须与**函数返回类型**一致（I4 的"终结符匹配"）。
    //   Sema 对 `return;`（非 void）与 `return expr;`（void）**已经报过错**，
    //   但 IRGen 面对的是"上游报过错、仍要产出合法 IR"的局面（§九.6：不许静默
    //   产出坏 IR）⇒ 这里按当前函数的返回类型**补齐或丢弃**：
    //     * 非 void 且没有值  ⇒ 补一个该类型的零值
    //     * 非 void 且类型不符 ⇒ 插一次转换（Sema 的 `Cast` 之外的第二道保险）
    //     * void 且带了值      ⇒ 丢弃那个值（求值副作用保留）
    const auto* s = static_cast<const ReturnStmt*>(n);
    std::vector<Value> ops;
    const bool voidFn = (curRetType_ == nullptr || curRetType_->kind == TypeKind::Void);
    if (!voidFn) {
      Value v = (s->value != nullptr) ? genExpr(s->value.get(), depth) : nullptr;
      ops.push_back(coerceTo(v, curRetType_, s->loc));
    } else if (s->value != nullptr) {
      (void)genExpr(s->value.get(), depth);   // 求值（副作用），但不作为操作数
    }
    terminator(OpKind::Return, ops, s->loc);
    return;
  }
  // 未知语句（残缺树）：Parser/Sema 已报过错；这里不产 Op，也不静默吞掉语句
  // —— 因为"空语句"本身就是合法的 SysY（`;`），没有可产出的 Op。
}

// 【后置】保证返回值**非空**：`v == nullptr` 时按 `fallbackTy` 补一个零值。
//   用途：残缺树（`int x = f();` 而 `f` 是 void —— Sema 已报 E-VOID-VALUE）
//   不许产出"操作数为空"的坏 IR（use-def 一致是六条不变式之一）。
Value Gen::orZero(Value v, const sysy::Type* fallbackTy, SourceLoc loc) {
  if (v != nullptr) return v;
  return (fallbackTy != nullptr && fallbackTy->kind == sysy::TypeKind::Float)
             ? cFlt(0u, loc)
             : cInt(0, loc);
}

// 【后置】把值 `v` 变成"返回类型 `want` 的值"：类型不符就插一次转换，
//   `v == nullptr` 就补该类型的零值。**只在"上游已报错"的路径上用到** ——
//   它的存在是为了让 IRGen 永不产出"ReturnOp 操作数个数与签名不符"的坏 IR
//   （Sema 对正常程序已经保证了类型正确，这里不会改变任何合法程序的 IR）。
Value Gen::coerceTo(Value v, const Type* want, SourceLoc loc) {
  if (want == nullptr || want->kind == TypeKind::Void) return nullptr;
  if (v == nullptr) return want->kind == TypeKind::F32 ? cFlt(0u, loc) : cInt(0, loc);
  const Type* have = (v->definer != nullptr && v->index == 0 && v->type != nullptr) ? v->type
                                                                                    : nullptr;
  if (have == want) return v;
  if (have != nullptr) {
    if (have->kind == TypeKind::I32 && want->kind == TypeKind::F32) {
      return un(OpKind::I2F, f32(), v, loc);
    }
    if (have->kind == TypeKind::F32 && want->kind == TypeKind::I32) {
      return genSatFptosi(v, loc);
    }
  }
  return v;   // 其它情形（指针/数组）：保守原样传（Sema 已报过错）
}

bool Gen::terminates(const Node* n) const {
  if (n == nullptr) return false;
  const std::string_view k = n->nodeKind();
  if (k == "ReturnStmt" || k == "BreakStmt" || k == "ContinueStmt") return true;
  if (k == "BlockStmt") {
    // 块：最后一条**可达**语句是否终止（前面的终止语句之后的都是死代码）
    const auto* b = static_cast<const BlockStmt*>(n);
    for (const auto& it : b->items) {
      if (it == nullptr) continue;
      if (it->nodeKind() == std::string_view("Decl")) continue;   // 声明不终止
      if (terminates(it.get())) return true;
    }
    return false;
  }
  if (k == "IfStmt") {
    const auto* s = static_cast<const IfStmt*>(n);
    return s->elseS != nullptr && terminates(s->thenS.get()) && terminates(s->elseS.get());
  }
  return false;
}

void Gen::genDecl(const Decl& d, int depth) {
  for (const auto& v : d.defs) {
    if (v != nullptr) genVarDef(*v);
  }
  (void)depth;
}

void Gen::genVarDef(const VarDef& v) {
  // ★ prompt §五.1：变量一律在内存 —— 每个 VarDef 一个 AllocaOp（入口 Region）。
  Value slot = emitAlloca(toIrType(v.semType), v.loc);
  syms_.emplace_back(v.name, Sym{slot, v.semType, false});
  // ── 初始化：**完全按 S04 的 InitPlan 走**（prompt §五.11），不重新解释 InitVal ──
  //   ★ 用 `localIdx_.take()`（同名队列 + 游标），**不**按名字直接查 ——
  //     同名遮蔽时按名字查会把内层变量配到外层的初始化值（实测的真 bug，见
  //     IRGenInternal.h 里 `LocalIndex` 的说明）。
  size_t liIdx = 0;
  if (localIdx_.take(funcName_ + "/" + v.name, liIdx)) {
    const sysy::InitPlan::LocalInit& li = plan_.locals[liIdx];
    const sysy::Type* elemSem = li.type.isArray() ? li.type.elem : &li.type;
    const Type* elemTy = toIrType(elemSem);
    // ★ 哨兵：**用"形状相等"判错配，而不是用 offset 范围**。
    //   ⚠️ 这里踩过一次：第一版查"每个动作的 offset 是否落在**本对象**范围内"，
    //      于是把**合法**的多动作计划（`Zero 0 4` + `MemcpyConst 4 …`）全判成越界
    //      —— 因为 offset 是"这一段写在哪里"，不是"整条计划的作用域"，
    //      判定必须用**整个计划的总跨度**（见下）。26 个文件被误报。
    //   正确判据：**本次声明的对象字节数 == 计划记录的字节数**
    //     （`InitPlan::locals` 的 `type` 是它给那个对象算出来的完整类型）。
    //     同名遮蔽时"第 k 条同名记录"与"第 k 次同名声明"必然同型；
    //     一旦错配（例如数组被当成标量），两者的大小几乎必然不同。
    const int64_t wantBytes = typeByteSize(toIrType(v.semType));
    const int64_t gotBytes = typeByteSize(toIrType(&li.type));
    if (wantBytes > 0 && gotBytes > 0 && wantBytes != gotBytes) {
      diag_.report(DiagLevel::Error, v.loc, kIrDiag,
                   "初始化计划与声明错配：`" + v.name + "` 声明为 " + typeText(toIrType(v.semType)) +
                       "（" + std::to_string(wantBytes) + " 字节），但计划记录为 " +
                       typeText(toIrType(&li.type)) + "（" + std::to_string(gotBytes) +
                       " 字节）");
    }
    for (const InitAction& a : li.actions) genAction(a, slot, elemTy, v.loc);
  }
}

// ============================================================================
// 初始化动作的降级（**输入是 InitPlan 的动作，不再看 InitVal**）
// ============================================================================
void Gen::genAction(const InitAction& a, Value slot, const Type* elemTy, SourceLoc loc) {
  switch (a.kind) {
    case InitActionKind::Zero:
      emitZero(slot, static_cast<int64_t>(a.bytes), loc);
      return;
    case InitActionKind::StoreConst: {
      // offset 是**字节偏移**（InitPlan 的契约）；GEP 的步长是元素 ⇒ 除以宽度
      const int64_t esz = typeByteSize(elemTy);
      const int64_t idx = esz > 0 ? static_cast<int64_t>(a.offset) / esz : 0;
      Value p = gep(elemTy, slot, sextI64(cInt(static_cast<int32_t>(idx), loc), loc), loc);
      Value v = a.value.isFloat ? cFlt(a.value.bits(), loc) : cInt(a.value.i, loc);
      store(elemTy, v, p, loc);
      return;
    }
    case InitActionKind::StoreExpr: {
      const int64_t esz = typeByteSize(elemTy);
      const int64_t idx = esz > 0 ? static_cast<int64_t>(a.offset) / esz : 0;
      Value p = gep(elemTy, slot, sextI64(cInt(static_cast<int32_t>(idx), loc), loc), loc);
      // ⚠️ StoreExpr 的表达式**只在局部**出现（全局必须是常量表达式），
      //    且此时 `cur_` 是函数的当前位置（保证"初始化的副作用顺序 = 源码顺序"）。
      Value v = orZero(genExpr(a.expr, 0), nullptr, loc);   // 兜底零值（i32）
      store(elemTy, v, p, loc);
      return;
    }
    case InitActionKind::MemcpyConst:
      emitMemcpyConst(slot, a.values, loc);
      return;
  }
}

// `Zero n`：小段逐元素 store，大段 `llvm.memset`（prompt §五.11）。
//   ★ 判据是**字节数**（通用结构判据），不是"这是哪个用例"。
void Gen::emitZero(Value dst, int64_t bytes, SourceLoc loc) {
  if (bytes <= 0) return;
  if (bytes <= kZeroInlineBytes) {
    const int64_t n = bytes / 4;
    for (int64_t i = 0; i < n; ++i) {
      Value p = gep(i32(), dst, sextI64(cInt(static_cast<int32_t>(i), loc), loc), loc);
      store(i32(), cInt(0, loc), p, loc);
    }
    return;
  }
  emitMemset(dst, bytes, loc);
}

// `llvm.memset(ptr, i32 0, i64 n, i32 1)` —— 操作数约定见 StructuredIR.h 的
// `llvm.memcpy`/`llvm.memset` 说明（S07 按它发射）。
void Gen::emitMemset(Value dst, int64_t bytes, SourceLoc loc) {
  Value p32 = bitcast(ptrTo(i32()), dst, loc);
  Value n64 = sextI64(cInt(static_cast<int32_t>(bytes), loc), loc);
  std::vector<Value> args{p32, cInt(0, loc), n64, cInt(1, loc)};
  (void)callTo("llvm.memset", args, voidTy(), loc);
}

// `MemcpyConst`：常量池全局（`<const.N:type:data>`）+ `llvm.memcpy`。
//   常量池与目标同型（于是"元素个数"天然一致，`bytes` 由类型算出），
//   源指针一次 bitcast 成 `ptr[i32]` —— 与 memset 的目标指针同型。
void Gen::emitMemcpyConst(Value dst, const std::vector<ConstValue>& vals, SourceLoc loc) {
  if (vals.empty()) return;
  if (static_cast<int64_t>(vals.size()) > kMaxPoolElements) {
    // §九.6：任何"超限就放弃"的分支**必须先报 error**（不静默）。
    diag_.report(DiagLevel::Error, loc, kIrDiag,
                 "初始化常量段超过 " + std::to_string(kMaxPoolElements) +
                     " 个元素；已降级为逐元素 store（不是静默丢弃）");
    const Type* et = vals[0].isFloat ? f32() : i32();
    for (size_t i = 0; i < vals.size(); ++i) {
      Value p = gep(et, dst, sextI64(cInt(static_cast<int32_t>(i), loc), loc), loc);
      store(et, vals[i].isFloat ? cFlt(vals[i].bits(), loc) : cInt(vals[i].i, loc), p, loc);
    }
    return;
  }
  const bool isF = vals[0].isFloat;
  const Type* et = isF ? f32() : i32();
  const Type* poolTy = arrOf(et, static_cast<int64_t>(vals.size()));
  Op* pool = mk(OpKind::GlobalVar, loc);
  pool->addAttr(Attr::ofStr("<const." + std::to_string(poolSeq_++) + ">"));
  pool->addAttr(Attr::ofType(poolTy));
  pool->addAttr(Attr::ofStr("data"));
  Attr data;
  data.kind = Attr::Kind::Data;
  for (size_t i = 0; i < vals.size(); ++i) {
    data.data.emplace_back(static_cast<uint64_t>(i) * 4u,
                           vals[i].isFloat ? vals[i].bits()
                                           : static_cast<uint32_t>(vals[i].i));
  }
  pool->addAttr(std::move(data));
  globalOps_.push_back(pool);   // 挂到模块级（全局都在函数之前，顺序可断言）

  // 常量池是一个模块级命名实体：它的"值"要用 GetGlobalOp 取（它没有结果）
  Op* getPool = mk(OpKind::GetGlobal, loc);
  getPool->addAttr(Attr::ofStr(pool->strAttr(0)));
  getPool->addAttr(Attr::ofType(ptrTo(poolTy)));
  globalOps_.push_back(getPool);   // 与它的 GlobalVar 相邻（顺序：声明后紧跟引用）
  Value src = bitcast(ptrTo(i32()), getPool->addResult(ptrTo(poolTy)), loc);
  Value dst32 = bitcast(ptrTo(i32()), dst, loc);
  const int64_t bytes = typeByteSize(poolTy);
  Value n64 = sextI64(cInt(static_cast<int32_t>(bytes), loc), loc);
  std::vector<Value> args{dst32, src, n64, cInt(1, loc)};
  (void)callTo("llvm.memcpy", args, voidTy(), loc);
}

// ============================================================================
// 顶层：全局对象与函数
// ============================================================================
// 【后置】把全部全局对象收集到 `globalOps_`（**按源文件声明顺序**）。
//   不在这里 push 进 Region：模块 Region 的顺序是"全局在前、函数在后"，而函数
//   在遍历过程中还会追加常量池 ⇒ 顺序在 build() 里统一组装。
void Gen::genGlobals() {
  for (const sysy::InitPlan::GlobalData& g : plan_.globals) {
    // ★ 初始化常量已经**完全**由 InitPlan 决定（`allZero` / `nonzero`），
    //   IRGen 只做搬运：`:zero` 不许展开成数据（语料里有 2.16 亿元素的数组）。
    Op* op = mk(OpKind::GlobalVar, g.loc);
    op->addAttr(Attr::ofStr(g.name));
    op->addAttr(Attr::ofType(toIrType(&g.type)));
    op->addAttr(Attr::ofStr(g.allZero ? "zero" : "data"));
    if (!g.allZero) {
      Attr data;
      data.kind = Attr::Kind::Data;
      data.data.reserve(g.nonzero.size());
      for (const auto& kv : g.nonzero) {
        // 已经是"字节偏移 → 值"、**按偏移升序**（S04 的契约）；这里只把
        // ConstValue 拍成 32 位原始位（int 是 i32 的位模式、float 是 IEEE 位模式）。
        data.data.emplace_back(kv.first, kv.second.bits());
      }
      op->addAttr(std::move(data));
    }
    // ★ GlobalVar **没有结果**：它是"模块级命名实体"，由 GetGlobalOp 按名字引用。
    //   （给声明本身塞一个结果会让 dump 里出现一堆永不使用的 `%x.0` 名字。）
    globalOps_.push_back(op);
    // ★ 每个全局预建一个 `GetGlobalOp`（"引用形式"）：函数体里读全局时用它。
    //   本文件里**不**产生任何 `CallOp` 引用全局 —— 全局数组的初始化在 S04 的
    //   InitPlan 里已经是 `:zero` / `:data`，消费方（S07）按 GlobalVar 发射数据段。
    //   GetGlobalOp 的结果类型是 `ptr[对象类型]`，于是"全局数组"与"局部数组"
    //   在下标计算上走**同一条路径**。
    //   ⚠️ 它必须挂在**模块级**（与 GlobalVar 相邻）：函数体里的 Op 引用它的结果，
    //      而 S06/S07 按模块顺序分配 —— 挂在函数体里会让"引用的定义在另一个
    //      Region"，读回/展平都要跨 Region 查表（第一次就是这么写的，dump 里
    //      出现 `%?`，一眼可辨）。
    Op* get = mk(OpKind::GetGlobal, g.loc);
    get->addAttr(Attr::ofStr(g.name));
    get->addAttr(Attr::ofType(toIrType(&g.type)));
    Value ref = get->addResult(ptrTo(toIrType(&g.type)));
    globalOps_.push_back(get);
    globals_.emplace_back(g.name, GlobalRef{ref, &g.type});
  }
}

void Gen::genFunction(const FuncDef& f) {
  // ★ 保存调用方的 `cur_`（模块 Region）并在结束时恢复：`emit(fn)` 必须把
  //   FuncOp 放进**模块** Region，而不是函数自己的体 Region。这个坑在第一次
  //   冒烟测试里就暴露了 —— 症状是"module region 是空的、op 全跑到 body 里"。
  Region* callerRegion = cur_;
  funcName_ = f.name;
  curRetType_ = f.isVoid ? voidTy() : (f.retType == BType::Float ? f32() : i32());
  const size_t symMark = syms_.size();

  Region* body = mkRegion();
  funcEntry_ = body;
  cur_ = body;

  // ── 全局对象进作用域（名字 → GetGlobalOp 的结果）───────────────────────
  //   ★ 顺序：**全局先登记、形参后登记** ⇒ 形参遮蔽同名全局（SysY 允许遮蔽）。
  //   ★ 这里只是把"引用形式"放进符号表，**不**发射任何 Op 到函数体里：
  //     未被函数引用的全局不应该在函数里出现伪指令。
  for (const auto& gr : globals_) {
    syms_.emplace_back(gr.first, Sym{gr.second.slot, gr.second.objType, true});
  }

  // ── 形参：先 `GetArgOp`，再存进内存 ─────────────────────────────────────
  //   数组形参退化：形参本身**已经是指针值**，但按"变量一律在内存"的决策
  //   （prompt §五.1 + 后端不变量 2）仍然给它一个 alloca 槽，读出时 Load。
  //   ⇒ `a[i]` 的地址计算始终从"槽里的指针值"出发，与局部数组完全同构，
  //     不需要为形参单独开一条路径。
  for (size_t i = 0; i < f.params.size(); ++i) {
    const Param& p = f.params[i];
    const Type* pty = toIrType(p.semType);
    Op* ga = mk(OpKind::GetArg, p.loc);
    ga->addAttr(Attr::ofInt(static_cast<int64_t>(i)));
    ga->addAttr(Attr::ofType(pty));
    Value argv = ga->addResult(pty);
    body->push(ga);
    Value slot = emitAlloca(pty, p.loc);   // funcEntry_ == body ⇒ 写进入口
    store(pty, argv, slot, p.loc);
    syms_.emplace_back(p.name, Sym{slot, p.semType, false});
  }

  // ── 函数体 ──────────────────────────────────────────────────────────────
  if (f.body != nullptr) genStmt(f.body.get(), 0);

  // ── 掉出末尾 ⇒ `return 0`（prompt §五.9：规范说未定义，**我们定义为返回 0**
  //     —— 跨目标确定优先于"跟硬件走"）。只在末尾**没有**终结 Op 时补。──────
  if (body->empty() || !isTerminator(body->back()->kind)) {
    if (f.isVoid) {
      terminator(OpKind::Return, {}, f.loc);
    } else {
      terminator(OpKind::Return, {cInt(0, f.loc)}, f.loc);
    }
  }

  Op* fn = mk(OpKind::Func, f.loc);
  fn->addAttr(Attr::ofStr(f.name));
  fn->addAttr(Attr::ofType(curRetType_));
  for (const Param& p : f.params) fn->addAttr(Attr::ofType(toIrType(p.semType)));
  fn->addRegion(body);

  // ★ 先把 `cur_` 恢复到**模块 Region**，再 emit FuncOp：FuncOp 属于模块，
  //   不属于函数体。顺序写成"显式两步"而不是靠 `emit` 之前的隐式状态 ——
  //   实测这个顺序在优化构建里被搅过一次（症状：FuncOp 落进体 Region，
  //   模块 Region 空），显式赋值把这类顺序假设彻底消掉。
  funcEntry_ = nullptr;
  cur_ = callerRegion;
  emit(fn);
  syms_.resize(symMark);   // 形参 + 体内声明的作用域退出
  funcName_.clear();
  poolSeq_ = 0;
}

Op* Gen::build(const CompUnit& unit, const std::string& baseName) {
  Region* modRegion = mkRegion();
  cur_ = modRegion;
  // ⚠️ 这里**不要** reset `localIdx_`：它在构造函数里按 `plan_.locals` 的顺序
  //    建好了"同名队列"。第一版在 build() 里 reset 了一次，把队列清空 ⇒
  //    **所有局部初始化都被静默跳过**（实测：`int x = 1;` 的 Store 消失，
  //    而轨 A/B/C 全绿 —— 只有"读 dump"才看得出来）。

  // ① 全局对象：只**收集**（不 push）—— 因为模块 Region 的顺序是
  //    "全局在前、函数在后"，而函数生成过程中还会追加常量池。
  genGlobals();

  // ② 函数：按源文件顺序生成（每个函数一个 FuncOp）。
  for (const auto& item : unit.items) {
    if (item == nullptr) continue;
    if (item->nodeKind() == std::string_view("FuncDef")) {
      genFunction(*static_cast<const FuncDef*>(item.get()));
    }
    // 顶层的 Decl 已经全部由 genGlobals 处理，这里不重复遍历。
  }

  // ③ 组装：**全局（含常量池）在前、函数在后** —— 顺序是可断言的性质。
  Region* body = mkRegion();
  for (Op* g : globalOps_) body->push(g);
  for (Op* fn : modRegion->ops()) body->push(fn);
  Op* mod = mk(OpKind::Module, SourceLoc());
  mod->addAttr(Attr::ofStr(baseName));
  mod->addRegion(body);
  return mod;
}


// ============================================================================
// 唯一对外入口（IRGen.h）
// ============================================================================
Op* buildModule(Arena& arena, const CompUnit& unit, const InitPlan& plan,
                const std::string& sourceBaseName, DiagnosticEngine& diag) {
  Gen gen(arena, plan, diag);
  return gen.build(unit, sourceBaseName);
}

}  // namespace sir
}  // namespace sysy
