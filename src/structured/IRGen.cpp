// ============================================================================
// IRGen.cpp —— 结构化 IR 生成：**Op 工厂 + 表达式**
//
//   接口与前置条件见 IRGen.h；实现私有细节见 IRGenInternal.h；
//   语句与初始化降级见 IRGenStmt.cpp（同一个类的成员函数，分文件是为了 §C4）。
//
// ── 铁律 1（前端禁止值传播）在这里的落点 ──────────────────────────────────
//   每一次**变量读出**都发一条 `LoadOp`：`int x = 1; return x;` 产出
//   `Alloca/Store 1/Load/Return %load`，**绝不**把 `Int 1` 直接喂给 return。
//   自研 mem2reg(S09) 的输入就是这些 load；前端"顺手优化"掉一个 load，
//   就等于把那条主线做废。`test_structured.cpp` 有一条形状断言钉死它。
// ============================================================================
#include "structured/IRGenInternal.h"

#include <cstring>

namespace sysy {
namespace sir {

// ============================================================================
// Op 工厂
// ============================================================================
Value Gen::cInt(int32_t v, SourceLoc loc) {
  Op* op = mk(OpKind::Int, loc);
  op->addAttr(Attr::ofInt(v));
  Value r = op->addResult(i32());
  // ★ 常量也是 **Op**，必须发射进当前 Region：否则 dump 里它没有"定义行"，
  //   而使用它的指令会打印出一个悬空引用（`%?`），读回器也会报 use-def 不一致。
  //   实测这是第一个冒烟测试抓出来的缺陷（dump 里全是 `%?`）。
  emit(op);
  return r;
}

Value Gen::cFlt(uint32_t bits, SourceLoc loc) {
  Op* op = mk(OpKind::Float, loc);
  op->addAttr(Attr::ofFBits(bits));
  Value r = op->addResult(f32());
  emit(op);   // 同上：常量是 Op，必须发射
  return r;
}

Value Gen::un(OpKind k, const Type* ty, Value a, SourceLoc loc) {
  Op* op = mk(k, loc);
  op->addOperand(a);
  Value r = op->addResult(ty);
  emit(op);
  return r;
}

Value Gen::bin(OpKind k, const Type* ty, Value a, Value b, SourceLoc loc) {
  Op* op = mk(k, loc);
  op->addOperand(a);
  op->addOperand(b);
  Value r = op->addResult(ty);
  emit(op);
  return r;
}

Value Gen::cmp(OpKind k, Value a, Value b, SourceLoc loc) {
  Op* op = mk(k, loc);
  op->addOperand(a);
  op->addOperand(b);
  Value r = op->addResult(i32());   // 比较结果一律 i32
  emit(op);
  return r;
}

Value Gen::emitAlloca(const Type* objTy, SourceLoc loc) {
  Op* op = mk(OpKind::Alloca, loc);
  op->addAttr(Attr::ofType(objTy));
  Value v = op->addResult(ptrTo(objTy));
  // ★ prompt §五.1：alloca 一律写**函数入口 Region**（S05b 的"提升"因此
  //   "位置不动"；后端/平面层可以依赖"所有 alloca 都在入口"这条不变量）。
  if (funcEntry_ != nullptr) funcEntry_->push(op);
  else emit(op);
  return v;
}

Value Gen::load(const Type* ty, Value p, SourceLoc loc) {
  Op* op = mk(OpKind::Load, loc);
  op->addAttr(Attr::ofType(ty));
  op->addOperand(p);
  Value r = op->addResult(ty);
  emit(op);
  return r;
}

void Gen::store(const Type* ty, Value v, Value p, SourceLoc loc) {
  Op* op = mk(OpKind::Store, loc);
  op->addAttr(Attr::ofType(ty));
  op->addOperand(v);
  op->addOperand(p);
  emit(op);
}

Value Gen::gep(const Type* elemTy, Value base, Value iv, SourceLoc loc) {
  Op* op = mk(OpKind::GetElementPtr, loc);
  op->addAttr(Attr::ofType(elemTy));
  op->addAttr(Attr::ofType(i64()));   // 下标类型 = **i64**（i32 里算完再 sext）
  op->addAttr(Attr::ofInt(0));        // I6 标记：0 = affine（IRGen 只产仿射）
  op->addOperand(base);
  op->addOperand(iv);
  Value r = op->addResult(ptrTo(elemTy));
  emit(op);
  return r;
}

Value Gen::bitcast(const Type* dst, Value p, SourceLoc loc) {
  Op* op = mk(OpKind::Bitcast, loc);
  op->addAttr(Attr::ofType(dst));
  op->addOperand(p);
  Value r = op->addResult(dst);
  emit(op);
  return r;
}

Value Gen::callTo(const std::string& callee, const std::vector<Value>& args,
                  const Type* ret, SourceLoc loc) {
  Op* op = mk(OpKind::Call, loc);
  op->addAttr(Attr::ofStr(callee));
  for (Value a : args) op->addOperand(a);
  Value r = nullptr;
  if (ret != nullptr && ret->kind != TypeKind::Void) r = op->addResult(ret);
  emit(op);
  return r;
}

void Gen::terminator(OpKind k, const std::vector<Value>& ops, SourceLoc loc) {
  Op* op = mk(k, loc);
  for (Value a : ops) op->addOperand(a);
  emit(op);
}

Value Gen::sextI64(Value v, SourceLoc loc) { return un(OpKind::Sext, i64(), v, loc); }

bool Gen::depthOk(int depth, SourceLoc loc) {
  if (depth <= kDepthLimit) return true;
  if (!overflow_) {
    overflow_ = true;
    diag_.report(DiagLevel::Error, loc, kIrDiag,
                 std::string("IR 生成深度超过 ") + std::to_string(kDepthLimit) +
                     " 层；该子树被保守降级（**不是静默丢弃**：这条诊断就是证据）");
  }
  return false;
}

const Sym* Gen::findSym(const std::string& name) const {
  for (size_t i = syms_.size(); i-- > 0;) {
    if (syms_[i].first == name) return &syms_[i].second;
  }
  return nullptr;
}

// ============================================================================
// 左值 / 下标
// ============================================================================
Value Gen::genLValAddr(const LVal& lv, int depth) {
  const Sym* s = findSym(lv.name);
  if (s == nullptr) return cInt(0, lv.loc);   // Sema 已报 E-UNDEF：给确定值，不崩

  const sysy::Type* objTy = s->objType;
  const Value base = s->slot;
  // 无下标 ⇒ 整个对象（或整个子数组）：直接返回它的地址。
  if (lv.indices.empty()) return base;
  // ★ 数组形参**与局部数组共用这一条路径**：`s->objType` 对形参已经退化成
  //   "指针所指对象"的类型（`int[]` → i32、`int[][5]` → `[5 x i32]`），
  //   对局部数组就是数组对象本身。于是下标链的生成只有一份实现。
  return genIndexChain(objTy, lv, depth, base);
}

// 逐维下标：**下标运算在 i32 里做**（会回绕，铁律 6），再 `sext i32→i64`，
// 最后 GEP（prompt §五.2）。每过一维发一次 sext —— 这样"负下标/回绕"都留在
// i32 语义里，与 SysY 的 int 语义一致（把偏移算成 i64 再相加就会掩盖回绕）。
//
// 【两处必须精确的地方】（第一版都写错了，被"读 dump"抓出来）
//   ① **指针的类型**：`p` 指向的是"当前对象"，所以 GEP 的元素类型必须是
//      `ptr[当前对象类型]`（`int[10]` 上取一次 = `ptr[[10 x i32]]`）。
//      写成"元素类型"（`i32`）会让 `b[i][j]` 的后续下标算错 —— 因为 `b[i]`
//      本应是"指向 int[3] 的指针"，却被标成了"指向 i32 的指针"。
//   ② **步长**：取 `a[k]` 要走"去掉第 k 维之后剩余维度的元素总数"，即
//      `elementCount(当前对象的元素类型)`。`int[2][3]` 上 `a[i]` 的步长是 **3**。
//      写死 1 会让 `a[i]` 落到错误的位置。
Value Gen::genIndexChain(const sysy::Type* startObjTy, const LVal& lv, int depth,
                         Value base) {
  const sysy::Type* curTy = startObjTy;   // 当前对象（`p` 指向的东西）的类型
  Value p = base;
  for (size_t k = 0; k < lv.indices.size(); ++k) {
    const Expr* idx = lv.indices[k].get();
    if (curTy == nullptr || !curTy->isArray()) {
      // 已经剥到标量：多出来的下标是 Sema 已报的 E-ARRAY-RANK；保守再走一层。
      Value iv = genExpr(idx, depth);
      p = gep(i32(), p, sextI64(iv, idx->loc), idx->loc);
      continue;
    }
    const sysy::Type* elemSemTy = curTy->elem;   // 取一次下标之后的"对象"
    // ⚠️ 这里要的是"指向**下一个对象**的指针" ⇒ 元素类型必须用
    //   `toIrObjType`（对象类型），不能用 `toIrType` —— 后者对数组会再包一层
    //   指针，于是 `int[2][1][3]` 的 `c[i]` 变成 `ptr[ptr[...]]`、步长/元素类型
    //   层层错位（实测 `.work/md2.sy` 两种形状都算 0，gcc 是 81）。
    const Type* ptrTy = ptrTo(toIrObjType(elemSemTy));
    const int64_t stride = elementCount(elemSemTy);
    Value iv = genExpr(idx, depth);
    if (stride > 1) {
      iv = bin(OpKind::MulI, i32(), iv, cInt(static_cast<int32_t>(stride), idx->loc),
               idx->loc);
    }
    p = gep(ptrTy, p, sextI64(iv, idx->loc), idx->loc);
    curTy = elemSemTy;
  }
  return p;
}

Value Gen::genLValValue(const LVal& lv, int depth) {
  Value addr = genLValAddr(lv, depth);
  const sysy::Type* vt = lv.type;
  if (vt != nullptr && vt->isArray()) return addr;   // 数组 → 地址（不 Load）
  return load(toIrType(vt), addr, lv.loc);
}

// ============================================================================
// 表达式
// ============================================================================
Value Gen::genExpr(const Expr* e, int depth) {
  if (e == nullptr) return cInt(0, SourceLoc());
  if (!depthOk(depth, e->loc)) return cInt(0, e->loc);
  return genExprRec(e, depth + 1);
}

Value Gen::genExprRec(const Expr* e, int depth) {
  const std::string_view k = e->nodeKind();
  if (k == "IntLit") {
    // ★ 字面量的**解析权在 ConstEvaluator**（S03 的唯一权威）。这里只是
    //   "把它算出来" —— 绝不重新实现进制判定。
    const auto* lit = static_cast<const IntLit*>(e);
    int32_t v = 0;
    std::string err;
    if (ConstEvaluator::parseIntLit(*lit, v, err)) return cInt(v, e->loc);
    return cInt(static_cast<int32_t>(lit->value), e->loc);   // Sema 已报过错
  }
  if (k == "FloatLit") {
    const auto* lit = static_cast<const FloatLit*>(e);
    float f = 0.0f;
    std::string err;
    uint32_t bits = 0;
    if (ConstEvaluator::parseFloatLit(*lit, f, err)) {
      std::memcpy(&bits, &f, sizeof(bits));
    }
    return cFlt(bits, e->loc);
  }
  if (k == "LVal") return genLValValue(*static_cast<const LVal*>(e), depth);
  if (k == "Call") return genCall(*static_cast<const Call*>(e), depth);
  if (k == "Cast") return genCast(*static_cast<const Cast*>(e), depth);
  if (k == "Unary") {
    const auto* u = static_cast<const Unary*>(e);
    const bool f = isFloat(u->operand->type);
    Value v = genExpr(u->operand.get(), depth);
    switch (u->op) {
      case TokKind::Plus:
        return v;
      case TokKind::Minus:
        // ★ 取负**不引入新 opcode**：整数用 `0 - x`（二进制补码下对 INT_MIN
        //   也正确，正好是回绕语义：-INT_MIN == INT_MIN）；浮点用 MinusF
        //   （S07 发射 LLVM 的 `fneg`，在声明的指令集里）。
        return f ? un(OpKind::MinusF, f32(), v, u->loc)
                 : bin(OpKind::SubI, i32(), cInt(0, u->loc), v, u->loc);
      case TokKind::Not:
        // Sema 只在条件上下文里接受 `!`；语义是"值 == 0"。
        return f ? cmp(OpKind::Eq, v, cFlt(0u, u->loc), u->loc)
                 : cmp(OpKind::Eq, v, cInt(0, u->loc), u->loc);
      default:
        return v;
    }
  }
  if (k == "Binary") return genBinary(*static_cast<const Binary*>(e), depth);
  return cInt(0, e->loc);   // 未知节点（残缺树）：给确定值，Sema 已报过错
}

Value Gen::genBinary(const Binary& b, int depth) {
  const bool f = isFloat(b.lhs->type) || isFloat(b.rhs->type);
  const Type* ty = f ? f32() : i32();
  const SourceLoc loc = b.loc;
  switch (b.op) {
    case TokKind::AmpAmp:
    case TokKind::PipePipe:
      return genLogical(b, depth);
    case TokKind::Plus:
      return bin(f ? OpKind::AddF : OpKind::AddI, ty, genExpr(b.lhs.get(), depth),
                 genExpr(b.rhs.get(), depth), loc);
    case TokKind::Minus:
      return bin(f ? OpKind::SubF : OpKind::SubI, ty, genExpr(b.lhs.get(), depth),
                 genExpr(b.rhs.get(), depth), loc);
    case TokKind::Star:
      return bin(f ? OpKind::MulF : OpKind::MulI, ty, genExpr(b.lhs.get(), depth),
                 genExpr(b.rhs.get(), depth), loc);
    case TokKind::Slash: {
      Value a = genExpr(b.lhs.get(), depth);
      Value c = genExpr(b.rhs.get(), depth);
      return f ? bin(OpKind::DivF, f32(), a, c, loc) : genNormDiv(a, c, loc);
    }
    case TokKind::Percent: {
      // `%` 只对 int 合法（Sema 已报 E-MOD-FLOAT）；float 情形保守按 int 降级。
      return genNormRem(genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), loc);
    }
    case TokKind::Less:
      return genCmp(OpKind::Lt, genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), f, loc);
    case TokKind::Greater:
      return genCmp(OpKind::Gt, genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), f, loc);
    case TokKind::LessEq:
      return genCmp(OpKind::Le, genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), f, loc);
    case TokKind::GreaterEq:
      return genCmp(OpKind::Ge, genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), f, loc);
    case TokKind::EqEq:
      return genCmp(OpKind::Eq, genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), f, loc);
    case TokKind::NotEq:
      return genCmp(OpKind::Ne, genExpr(b.lhs.get(), depth), genExpr(b.rhs.get(), depth), f, loc);
    default:
      return cInt(0, loc);
  }
}

// 比较：结果一律 i32。**浮点的谓词是有序/无序版本**（S07 的映射表）：
//   == → oeq · != → une · < → olt · <= → ole · > → ogt · >= → oge
// ★ 这里**不做**"交换操作数把 Gt 变成 Lt"之类的改写（那是优化，§十一 第 4 条）。
Value Gen::genCmp(OpKind k, Value a, Value b, bool floatOp, SourceLoc loc) {
  (void)floatOp;   // 谓词由 kind + 操作数类型共同决定，S07 按操作数类型选谓词
  return cmp(k, a, b, loc);
}

// `x / y` 的归一化守卫（prompt §五.4：**调 S03 导出的语义，不重写**）
//   `y == 0`                  → 0
//   `x == INT_MIN ∧ y == -1`  → 0
//   其余                       → `x sdiv y`（**不加 nsw**）
// ⚠️ 常量折叠（ConstEvaluator::normDivInt）与这里必须是**同一个答案**。
Value Gen::genNormDiv(Value a, Value b, SourceLoc loc) {
  Value isZero = cmp(OpKind::Eq, b, cInt(0, loc), loc);
  Value intMin = cmp(OpKind::Eq, a, cInt(INT32_MIN, loc), loc);
  Value negOne = cmp(OpKind::Eq, b, cInt(-1, loc), loc);
  Value isOvf = bin(OpKind::MulI, i32(), intMin, negOne, loc);   // 布尔与（int 1/0）
  Value bad = bin(OpKind::AddI, i32(), isZero, isOvf, loc);      // 两者不可能同时为真
  return genConditionalI32(
      bad, [&]() { (void)cInt(0, loc); },                         // then：结果 0
      [&]() { (void)bin(OpKind::DivI, i32(), a, b, loc); },       // else：真正的商
      loc);
}

// `x % y` 的归一化（prompt §五.4）：
//   `y == 0`      → **结果 x**（实测两个目标一致，所以只需一条守卫）
//   `INT_MIN % -1`→ 0
//   其余           → `x srem y`（**不加 nsw**）
Value Gen::genNormRem(Value a, Value b, SourceLoc loc) {
  Value isZero = cmp(OpKind::Eq, b, cInt(0, loc), loc);
  Value intMin = cmp(OpKind::Eq, a, cInt(INT32_MIN, loc), loc);
  Value negOne = cmp(OpKind::Eq, b, cInt(-1, loc), loc);
  Value isOvf = bin(OpKind::MulI, i32(), intMin, negOne, loc);
  Value bad = bin(OpKind::AddI, i32(), isZero, isOvf, loc);
  return genConditionalI32(
      bad,
      [&]() {
        // bad 里要区分"除零 ⇒ x"与"INT_MIN%-1 ⇒ 0"
        (void)genConditionalI32(
            isZero, [&]() { (void)a; }, [&]() { (void)cInt(0, loc); }, loc);
      },
      [&]() { (void)bin(OpKind::ModI, i32(), a, b, loc); }, loc);
}

// `x && y` / `x || y` 的**短路**求值：右侧只在需要时求值。
//   `&&`：`if (boolOf(lhs)) r = boolOf(rhs) else r = 0`
//   `||`：`if (boolOf(lhs)) r = 1          else r = boolOf(rhs)`
//   ★ 与 Sema 的语义一致（两侧已被 ToBool 包住，结果是 int）；
//     **短路是靠 Region 结构保证的**，不是靠布尔运算的代数性质。
Value Gen::genLogical(const Binary& b, int depth) {
  const SourceLoc loc = b.loc;
  Value lhs = genExpr(b.lhs.get(), depth);
  Value lhBool = isFloat(b.lhs->type) ? cmp(OpKind::Ne, lhs, cFlt(0u, loc), loc)
                                      : cmp(OpKind::Ne, lhs, cInt(0, loc), loc);
  const bool isAnd = (b.op == TokKind::AmpAmp);
  return genConditionalI32(
      lhBool,
      [&]() {
        if (isAnd) {
          Value rhs = genExpr(b.rhs.get(), depth);
          (void)(isFloat(b.rhs->type) ? cmp(OpKind::Ne, rhs, cFlt(0u, loc), loc)
                                      : cmp(OpKind::Ne, rhs, cInt(0, loc), loc));
        } else {
          (void)cInt(1, loc);
        }
      },
      [&]() {
        if (isAnd) {
          (void)cInt(0, loc);
        } else {
          Value rhs = genExpr(b.rhs.get(), depth);
          (void)(isFloat(b.rhs->type) ? cmp(OpKind::Ne, rhs, cFlt(0u, loc), loc)
                                      : cmp(OpKind::Ne, rhs, cInt(0, loc), loc));
        }
      },
      loc);
}

// float→int 的**饱和**归一化（prompt §五.5；语义与 `satFptosi` 逐位一致）：
//   NaN → 0 · v > 2147483647.0f → INT_MAX · v < -2147483648.0f → INT_MIN
//   否则 → `fptosi`
// ★ 阈值就是 2^31（在 f32 里精确可表示）。
Value Gen::genSatFptosi(Value v, SourceLoc loc) {
  const float hiF = 2147483648.0f, loF = -2147483648.0f;
  uint32_t hiB = 0, loB = 0;
  std::memcpy(&hiB, &hiF, sizeof(hiB));
  std::memcpy(&loB, &loF, sizeof(loB));
  Value tooHigh = cmp(OpKind::Gt, v, cFlt(hiB, loc), loc);   // NaN ⇒ false
  Value tooLow = cmp(OpKind::Lt, v, cFlt(loB, loc), loc);    // NaN ⇒ false
  Value nanv = cmp(OpKind::Ne, v, v, loc);                   // NaN ⇒ true
  // 嵌套三层：NaN 时 Gt/Lt 都为假 ⇒ 只有 nanv 为真 ⇒ 一定落到 0（无需优先级假设）
  Value notHigh = genConditionalI32(
      tooHigh, [&]() { (void)cInt(INT32_MAX, loc); },
      [&]() {
        (void)genConditionalI32(
            tooLow, [&]() { (void)cInt(INT32_MIN, loc); },
            [&]() {
              (void)genConditionalI32(
                  nanv, [&]() { (void)cInt(0, loc); },
                  [&]() { (void)un(OpKind::F2I, i32(), v, loc); }, loc);
            },
            loc);
      },
      loc);
  return notHigh;
}

// ── 条件赋值的**唯一**降级方式（见类内声明处的说明）
void Gen::genYieldInto(Region* r, const std::function<void()>& body, SourceLoc loc) {
  Region* saved = cur_;
  cur_ = r;
  body();
  if (r->empty() || !isTerminator(r->back()->kind)) {
    terminator(OpKind::Yield, {}, loc);
  }
  cur_ = saved;
}

Value Gen::genConditionalI32(Value cond, const std::function<void()>& genThen,
                             const std::function<void()>& genElse, SourceLoc loc) {
  Value slot = emitAlloca(i32(), loc);
  Region* thenR = mkRegion();
  Region* elseR = mkRegion();
  // 两个 Region 都把"最后一个有结果的 Op"写进槽（没有 ⇒ 写 0，保守且确定）
  genYieldInto(thenR, [&]() {
    genThen();
    Op* last = thenR->empty() ? nullptr : thenR->back();
    Value v = (last != nullptr && last->numResults() > 0) ? last->result(0) : cInt(0, loc);
    store(i32(), v, slot, loc);
  }, loc);
  genYieldInto(elseR, [&]() {
    genElse();
    Op* last = elseR->empty() ? nullptr : elseR->back();
    Value v = (last != nullptr && last->numResults() > 0) ? last->result(0) : cInt(0, loc);
    store(i32(), v, slot, loc);
  }, loc);
  Op* ifOp = mk(OpKind::If, loc);
  ifOp->addOperand(cond);
  ifOp->addRegion(thenR);
  ifOp->addRegion(elseR);
  emit(ifOp);
  return load(i32(), slot, loc);
}

Value Gen::genCast(const Cast& c, int depth) {
  Value v = genExpr(c.operand.get(), depth);
  switch (c.kind) {
    case CastKind::IntToFloat:
      return un(OpKind::I2F, f32(), v, c.loc);
    case CastKind::ToBool:
      // Sema 已把布尔位置收敛（prompt §五.6）：int → `icmp ne x, 0`；
      // float → `fcmp une x, 0.0`（**不是**截断：`(int)0.5 == 0` 但 `if (0.5)` 为真）
      return isFloat(c.operand->type) ? cmp(OpKind::Ne, v, cFlt(0u, c.loc), c.loc)
                                      : cmp(OpKind::Ne, v, cInt(0, c.loc), c.loc);
    case CastKind::FloatToInt:
      return genSatFptosi(v, c.loc);
  }
  return v;
}

Value Gen::genCall(const Call& c, int depth) {
  std::vector<Value> args;
  args.reserve(c.args.size());
  for (const auto& a : c.args) args.push_back(genExpr(a.get(), depth));
  // ★ prompt §五.8：`starttime()` / `stoptime()` 是**运行时名字表里的零参 void
  //   函数**，降级成 `_sysy_starttime(<调用点行号>)`。行号取 `Call::loc`（D11）。
  //   ⚠️ 判据是**名字表查询**（`lookupRuntimeFunc`），不是"按名字写 if"（铁律 2）：
  //      规则是通用的 —— "运行时名字表里存在 `_sysy<name>` 且该调用零实参" ⇒
  //      补一个行号实参。
  if (const FuncSig* sig = lookupRuntimeFunc(c.callee)) {
    if (sig->ret != nullptr && isVoid(sig->ret) && args.empty()) {
      const std::string rt = "_sysy" + c.callee;
      if (lookupRuntimeFunc(rt) == nullptr) {
        std::vector<Value> withLine(args);
        withLine.push_back(cInt(static_cast<int32_t>(c.loc.line), c.loc));
        return callTo(rt, withLine, voidTy(), c.loc);
      }
    }
  }
  return callTo(c.callee, args, toIrType(c.type), c.loc);
}


}  // namespace sir
}  // namespace sysy
