// ============================================================================
// structured/FlattenLower.cpp —— FlattenCFG 的**单条 Op 降级**实现
//
//   契约、φ 放置判据、临界边拆分的完整说明在 `FlattenCFG.h` 的文件头；
//   块骨架（If/While/For 的展开）在 `FlattenCFG.cpp`。本文件只回答一个问题：
//   **这一条结构化 Op 变成哪几条平面指令**。
//
// ── 两条纪律 ──────────────────────────────────────────────────────────────
//   ① **纯机械**：一个 Op → 固定形态的平面指令，不做任何判断/化简
//      （prompt §五 / 设计文档 §4.2）。唯一"多指令"的是 `F2I`：结构化层
//      的 `F2I` **已经**是"饱和转换"（铁律 6 的归一化），平面层必须保留
//      同一语义 ⇒ 用 `fcmp`+`select` 链展开，与结构化层
//      `genSatFptosi` 的 `if` 链是**同一套语义**。
//   ② **不丢 SourceLoc**（D11）：每条平面指令都带结构化 Op 的行号 ——
//      `_sysy_starttime` 的行号、以及"这条指令来自哪一行"的调试线索全靠它。
//
// ── `MinusI` / `MinusF` 怎么降级（prompt §五.8 点名的那条）────────────────
//   ⚠️ **`neg` 不是 LLVM 指令**（S00 实测踩过）。
//     * `MinusI x` → `sub 0, x`（一条 `sub`，第一个操作数是常量 0）；
//     * `MinusF x` → `fneg x`（`fneg` **是** LLVM 指令，在 `iset.txt` 里）。
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "structured/FlattenInternal.h"

namespace sysy {
namespace flat {
namespace {

// 【后置】i32 值对应的 f32 位模式（`memcpy` 不是 constexpr ⇒ 不能是 constexpr 函数）
uint32_t f32BitsFor(int32_t i) {
  float f = static_cast<float>(i);
  uint32_t b = 0;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

}  // namespace

// 【后置】`v` 是否由 `alloca` 定义（本地槽）。
//   为什么需要：判据 2 只覆盖"平面层变量"= `alloca` 出来的槽。
//   全局对象的地址（`GlobalAddr`）与常量池地址**不是**变量，
//   给它们维护版本会在每个汇合点造出无意义的 φ。
Value* FlatBuilder::rootSlot(Value* v) const {
  if (v == nullptr || !v->isInst()) return nullptr;
  Instruction* i = static_cast<Instruction*>(v);
  // 上限 64 步：正常形态只有 1–2 步（`gep` / `bitcast`），防止畸形输入成环
  for (int guard = 0; guard < 64; ++guard) {
    if (i == nullptr) return nullptr;
    if (i->op() == Opcode::Alloca) return i;
    if (i->op() != Opcode::GEP && i->op() != Opcode::BitCast) return nullptr;
    Value* base = i->operand(0);
    if (base == nullptr || !base->isInst()) return nullptr;
    i = static_cast<Instruction*>(base);
  }
  return nullptr;
}

bool FlatBuilder::amLocalSlot(Value* v) const {
  return v != nullptr && v->isInst() && static_cast<Instruction*>(v)->op() == Opcode::Alloca;
}

// ============================================================================
// 终结符
// ============================================================================
// 【后置】返回 true 表示"该 Region 到此为止"（终结符已发）。
//   `Yield` 的语义是**容器相关**的：
//     * while 的条件 Region：`Yield <bool>` ⇒ 在这里发 `br <bool>, body, exit`；
//     * 体 / 分支 Region：`Yield` ⇒ 跳到帧的续点（体→循环头或自增块；
//       分支→汇合块）；
//     * 循环体里的 `Break` ⇒ 跳到循环出口（**结构化层的 `Break` 就是
//       "跳出当前循环"**；`continue` 已由 S05b 的规范化消解掉）。
bool FlatBuilder::lowerTerminator(Op* op, Frame& fr) {
  if (op->kind == OpKind::Yield) {
    if (fr.isWhileCond) {
      // 条件 Region 的 `Yield`：它的操作数就是条件值
      Value* c = op->numOperands() > 0 ? map(op->operand(0)) : nullptr;
      if (c == nullptr) c = kInt(0, op->loc);   // 残缺 IR：保守按"假"（退出循环）
      cur_->addInst(createCondBr(c, fr.cont, fr.loopExit, op->loc));
      ownInst(cur_->back());
      return true;
    }
    if (fr.cont != nullptr && !hasTerm(cur_)) emit(createBr(fr.cont, op->loc));
    return true;
  }
  if (op->kind == OpKind::Break) {
    if (fr.loopExit == nullptr) {
      // 循环外出现 `Break`：IRGen 不会产出（Sema 已报 break-outside-loop）。
      // 保守降级成 `unreachable` 并**留痕**，不静默产出坏 IR。
      giveUp("循环外出现 BreakOp（上游已报错？），降级为 unreachable", op->loc);
      emit(createUnreachable(op->loc));
      return true;
    }
    // 记下"这一刻的环境"：循环出口块的 φ 需要它（判据 2）
    breaks_.emplace_back(cur_, env_);
    emit(createBr(fr.loopExit, op->loc));
    return true;
  }
  if (op->kind == OpKind::Return) {
    Value* v = op->numOperands() > 0 ? map(op->operand(0)) : nullptr;
    if (v != nullptr) {
      cur_->addInst(createRet(v, op->loc));
    } else {
      cur_->addInst(createRet(nullptr, op->loc));
    }
    ownInst(cur_->back());
    return true;
  }
  if (op->kind == OpKind::Unreachable) {
    emit(createUnreachable(op->loc));
    return true;
  }
  if (op->kind == OpKind::Goto) {
    // 结构化层**不产出** `GotoOp`（`StructuredIR.h` 的注释：平面化的预留形态）。
    // 真遇到就报 error（不许静默）：本关不做"结构化层之外的输入"。
    giveUp("遇到 GotoOp（结构化层不应产出）", op->loc);
    emit(createUnreachable(op->loc));
    return true;
  }
  return false;
}

// ============================================================================
// 单条 Op（非终结符、非控制流容器）
// ============================================================================
// 【后置】把一个"在循环前块求值一次"的操作数取出来。
//   ⚠️ 为什么要单独做：S05b 把 `while (i < 4)` 升成 `For` 时，`<lower>` 用的
//   就是**条件里那条 `Load i`**，而它的平面映射此刻正好是**循环头的 IV φ**
//   （判据 2 把 IV 槽提升成了 φ）—— 于是 φ 的"进入边入值"变成 φ 自己
//   （自引用），支配检查立刻报 V2/V3。修法：在**循环前块**重发一条 load，
//   它读到的是循环开始前的槽值，这才是 `<lower>` 的真正来源。
Value* FlatBuilder::preheaderValue(sir::Value v, Value* phi, const Type* ty, BasicBlock* bb,
                                   SourceLoc loc) {
  Value* fv = map(v);
  if (fv != phi || phi == nullptr) return fv;
  // ⚠️ 这条 load 的指针操作数是 φ（指针类型正确），但它**必须挂在循环前块上**
  //   （`bb`，不是 `cur_` —— 建 `for.head` 的时候 `cur_` 已经改了）。
  Instruction* fresh = createLoad(ty != nullptr ? ty : typePool().i32(),
                                  const_cast<Value*>(static_cast<const Value*>(phi)), loc);
  ownInst(fresh);
  bb->addInst(fresh);
  return fresh;
}

Action FlatBuilder::lower(Op* op) {
  switch (op->kind) {
    // ── 常量 ──────────────────────────────────────────────────────────
    case OpKind::Int: {
      // ⚠️ **不要**写成 `bind(op->result(0), kInt(x, op->loc))`：
      //   C++ 的函数实参求值顺序**未指定**，而 `op->loc` 那次求值会先算
      //   `op->result(0)` —— 实测这条链在某些求值顺序下会踩到悬垂/半构造的
      //   结果（症状：常量被造出来了但**没登记进模块**，dump 打成 `%?`）。
      //   显式分步，顺序就是确定的。
      sir::Value r = op->result(0);
      const int64_t v = op->intAttr(0);
      Value* c = kInt(static_cast<int32_t>(v), op->loc);
      bind(r, c);
      return Action::kOk;
    }
    case OpKind::Float: {
      // 结构化层用 `Attr::Float`（IEEE-754 原始位）保证无损。
      sir::Value r = op->result(0);
      const sir::Attr* a = op->attr(0);
      const uint32_t bits = (a != nullptr && a->kind == sir::Attr::Kind::Float) ? a->fbits : 0u;
      bind(r, kFlt(bits, op->loc));
      return Action::kOk;
    }
    case OpKind::GetArg:
      // 形参在 `run()` 里已经建好并绑定；这里不需要再做任何事。
      return Action::kOk;
    case OpKind::GetGlobal: {
      GlobalVariable* g = m_.findGlobal(op->strAttr(0));
      if (g == nullptr) {
        giveUp("GetGlobal 引用了不存在的全局 `" + op->strAttr(0) + "`", op->loc);
        return Action::kOk;
      }
      bind(op->result(0), m_.globalAddr(g));
      return Action::kOk;
    }
    // ── 内存 ──────────────────────────────────────────────────────────
    case OpKind::Alloca: {
      const Type* objTy = toFlatType(op->typeAttr(0));
      if (objTy == nullptr) {
        giveUp("Alloca 的对象类型无法映射", op->loc);
        return Action::kOk;
      }
      Instruction* i = createAlloca(objTy, op->loc);
      bind(op->result(0), i);
      // 记下"平面槽 → 结构化 Alloca 结果"，供判据 2（`isReadSlot`）查询
      if (op->result(0) != nullptr) sirOfSlot_[i] = op->result(0);
      // ★ **alloca 一律进入口块**（平面层不变量 2 / 后端契约 2）。
      //   结构化层的验收口径是"alloca 在入口 **Region**"（S05b 的 AllocaHoist），
      //   但在入口 Region **中间的 `IfOp` 分支里**声明时，展平后它落在
      //   条件块里 —— 而分层的后果是"每进一次分支就在栈上再要一块内存"
      //   （大数组会爆栈、后端也没法一次性布局栈帧）。
      //   ⇒ 展平时统一改挂入口块（**不改变 alloca 之间的相对顺序**）。
      //   ⚠️ 这**不是**优化（铁律 1 不受影响：变量照旧 load/store），
      //      只是"内存分配点"的归一化，属于 S05b `AllocaHoist` 的同一条规则。
      if (cur_ != f_->entry()) {
        //   ⚠️ 用 `insertInst(0, …)`：调用点先建的块（入口 → …）先被降级，
        //     所以"反复插到最前"得到的就是**原有相对顺序**（等于追加）。
        //     绝不能 `addInst`（那会插到终结符之后 ⇒ "终结符不是最后一条"）。
        f_->entry()->insertInst(0, i);
        ownInst(i);
      } else {
        emit(i);
      }
      return Action::kOk;
    }
    case OpKind::Load: {
      Value* p = op->numOperands() > 0 ? map(op->operand(0)) : nullptr;
      // ── 读出来的类型怎么定（一个**必须想清楚**的点）────────────────────
      //   结构化层的 `LoadOp` 带 `<ty>`（"读出来是什么"），**绝大多数情况是对的**，
      //   只有一种形态是错的：**数组形参 / 数组对象的 `Load`**。
      //   那种形态在平面层有一个**可判定的特征**：地址是"假指针 GEP"
      //   —— `GetElementPtr <ty> … <base> 0`，而 `<ty>` 与 `base` 的元素类型
      //      **不同**（结构化层的 GEP 是"带类型的偏移"，不要求两者相同）。
      //     此时结构化层的 `<ty>` 写的是 `i32`（它把数组下标当标量看待），
      //      而**平铺语义要求的**是：在槽的地址上 load 出"槽里装的那根指针"
      //      ⇒ 类型 = **基址指针的元素类型**。
      //   例（`pick(int a[], int i)`）：
      //     `(GetElementPtr %g ptr[i32] i64 0 %a_slot %i)` ⇒ 基址 `ptr[ptr[i32]]`
      //     ⇒ `load` 的类型是 `ptr[i32]`（不是 `i32`），这正是 `a[i]` 的基址。
      //   直接 `load` 槽（`p` 不是 GEP）时结构化层的 `<ty>` 是对的
      //   （局部数组对象读 `i32` 元素、标量槽读标量）。
      const Type* ty = toFlatType(op->typeAttr(0));
      if (p != nullptr && p->isInst()) {
        Instruction* pi = static_cast<Instruction*>(p);
        if (pi->op() == Opcode::GEP) {
          Value* base = pi->operand(0);
          const Type* et = pi->srcElemType();
          const Type* bt = base != nullptr ? base->type() : nullptr;
          if (bt != nullptr && bt->isPtr() && et != bt->elem) {
            ty = bt->elem;   // "假指针 GEP"：读出来是基址指向的那个对象
          }
        }
      }
      if (ty == nullptr || p == nullptr) {
        giveUp("Load 的操作数/类型缺失", op->loc);
        return Action::kOk;
      }
      // ★ 判据 2 的"读"：命中环境 ⇒ 用环境里的值（**不再发 load**）。
      //   这不是值传播：环境里的值是"同一个槽在这次控制流上的当前版本"，
      //   与 mem2reg 的 rename 同构；被提升的只有"汇合处必须选择"的那些值。
      //   ⚠️ 只对**本地槽**（`alloca` 出来的）查环境：全局对象的地址
      //   （`GlobalAddr`）永远走真正的 `load` —— 后端的全局变量语义就是内存。
      if (Value* root = rootSlot(p)) {
        if (!ptrSlots_.count(root)) {
          const auto it = env_.find(root);
          if (it != env_.end()) {
            bind(op->result(0), it->second);
            return Action::kOk;
          }
        }
      }
      Instruction* i = createLoad(ty, p, op->loc);
      emit(i);
      bind(op->result(0), i);
      return Action::kOk;
    }
    case OpKind::Store: {
      const Type* ty = toFlatType(op->typeAttr(0));
      Value* v = op->numOperands() > 0 ? map(op->operand(0)) : nullptr;
      Value* p = op->numOperands() > 1 ? map(op->operand(1)) : nullptr;
      if (ty == nullptr || v == nullptr || p == nullptr) {
        giveUp("Store 的操作数/类型缺失", op->loc);
        return Action::kOk;
      }
      // 地址是**全局**（常量池 / 全局变量）⇒ 变量不在内存里被改名，
      //   必须发真正的 store（判据 2 只覆盖 `alloca` 出来的局部变量）。
      const bool isLocalSlot = (rootSlot(p) != nullptr);
      if (!isLocalSlot) {
        Instruction* i = createStore(ty, v, p, op->loc);
        emit(i);
        return Action::kOk;
      }
      // ★ 指针/数组值的槽**不参与环境提升**（判据 2 的例外，见 FlattenInternal.h）：
      //   把"数组形参的槽"提升成"参数值本身"会改变 IR 形状（`load` 变成直接用
      //   参数、类型也跟着错），而结构化层对这种槽的 `load` 类型属性又是错的。
      //   本关一律不提升它们 —— 它们照旧 load/store，S09 的 mem2reg 会处理。
      if (Value* root = rootSlot(p)) {
        const Type* vt = v->type();
        if (vt != nullptr && (vt->isPtr() || vt->isArray())) ptrSlots_.insert(root);
      }
      // 局部槽：更新环境即完成"写"。
      //   ⚠️ 值相同（`store x, slot` 而槽里已经是 x）时**仍然发 store**：
      //   "顺手删掉一个 store"属于 DCE/S09 的活（铁律 1 的反面同样成立 ——
      //   本关不做任何优化，prompt §十.3）。
      setSlot(p, v);
      Instruction* i = createStore(ty, v, p, op->loc);
      emit(i);
      return Action::kOk;
    }
    case OpKind::GetElementPtr: {
      // ── 元素类型从哪来（**这里有一个必须绕开的坑**）────────────────────
      //   结构化层里 `GetElementPtr` 的 `<ty>` 属性**在往返之后会多包一层
      //   `ptr[...]`**：`StructuredReader::resultTypeOf` 在**解析属性之前**
      //   就被调用来定结果类型，于是 `at(0)` 还是 nullptr、`ptrTo(nullptr)`
      //   成了 `%pick.6` 的结果类型；而那个错误的类型又被用在
      //   "取操作数元素类型"的地方（`arithOf`/`checkGep` 的推导链）。
      //   证据：`09-arrays` 的 `pick` 在 IRGen 直出的 dump 里是
      //   `(GetElementPtr i32 ptr[i32] 0 %a_slot %i)`，而往返之后同一条变成
      //   `(GetElementPtr ptr[i32] ptr[i32] 0 …)` ⇒ 只可能是读回时被改写的。
      //   ⚠️ 冻结契约要求"往返逐字节相同"，所以**不能改 dump 或读回器**；
      //      展平侧按下面的规则还原：**基址指向一个标量、而 `<ty>` 是个指针**
      //      ⇒ 那次"指针"不是元素的类型，取**基址的元素类型**
      //      （`int a[]` 的 `a[i]`：基址 `ptr[i32]`、`<ty>` 被污染成 `ptr[i32]`
      //       ⇒ 元素类型是 `i32`）。
      //      为什么限定"标量"：基址指向 `[3 x i32]` 这类**聚合**时，
      //      `<ty>` 是"取一次下标之后的指针"（`&loc` ⇒ `ptr[[3 x i32]]`），
      //      那是正确的语义，不能动。
      const Type* elemTy = toFlatType(op->typeAttr(0));
      Value* base = op->numOperands() > 0 ? map(op->operand(0)) : nullptr;
      Value* idx = op->numOperands() > 1 ? map(op->operand(1)) : nullptr;
      if (elemTy != nullptr && base != nullptr && elemTy->isPtr() && elemTy->elem != nullptr) {
        const Type* bt = base->type();
        if (bt != nullptr && bt->isPtr() && bt->elem != nullptr && bt->elem->isScalar()) {
          elemTy = bt->elem;
        }
      }
      if (elemTy == nullptr || base == nullptr || idx == nullptr) {
        giveUp("GetElementPtr 的操作数/类型缺失", op->loc);
        return Action::kOk;
      }
      Instruction* i = createGEP(elemTy, base, idx, op->loc);
      emit(i);
      bind(op->result(0), i);
      return Action::kOk;
    }
    case OpKind::Bitcast: {
      const Type* dst = toFlatType(op->typeAttr(0));
      Value* p = op->numOperands() > 0 ? map(op->operand(0)) : nullptr;
      if (dst == nullptr || p == nullptr) {
        giveUp("Bitcast 的操作数/类型缺失", op->loc);
        return Action::kOk;
      }
      Instruction* i = createBitCast(dst, p, op->loc);
      emit(i);
      bind(op->result(0), i);
      return Action::kOk;
    }
    // ── 整数 ──────────────────────────────────────────────────────────
    case OpKind::AddI: case OpKind::SubI: case OpKind::MulI:
    case OpKind::DivI: case OpKind::ModI: {
      Value* a = map(op->operand(0));
      Value* b = map(op->operand(1));
      if (a == nullptr || b == nullptr) {
        giveUp("整数二元运算的操作数缺失", op->loc);
        return Action::kOk;
      }
      const Opcode k = (op->kind == OpKind::AddI)   ? Opcode::Add
                       : (op->kind == OpKind::SubI) ? Opcode::Sub
                       : (op->kind == OpKind::MulI) ? Opcode::Mul
                       : (op->kind == OpKind::DivI) ? Opcode::SDiv
                                                    : Opcode::SRem;
      Instruction* r = createBin(k, a, b, op->loc);
      { sir::Value rr = op->result(0); emit(r); bind(rr, r); }
      return Action::kOk;
    }
    case OpKind::MinusI: {
      // ★ `neg` 不是 LLVM 指令 ⇒ `sub 0, x`（prompt §五.8 点名的坑）
      Value* a = map(op->operand(0));
      if (a == nullptr) { giveUp("MinusI 的操作数缺失", op->loc); return Action::kOk; }
      Instruction* r = createBin(Opcode::Sub, kInt(0, op->loc), a, op->loc);
      { sir::Value rr = op->result(0); emit(r); bind(rr, r); }
      return Action::kOk;
    }
    // ── 浮点 ──────────────────────────────────────────────────────────
    case OpKind::AddF: case OpKind::SubF: case OpKind::MulF: case OpKind::DivF: {
      Value* a = map(op->operand(0));
      Value* b = map(op->operand(1));
      if (a == nullptr || b == nullptr) {
        giveUp("浮点二元运算的操作数缺失", op->loc);
        return Action::kOk;
      }
      const Opcode k = (op->kind == OpKind::AddF)   ? Opcode::FAdd
                       : (op->kind == OpKind::SubF) ? Opcode::FSub
                       : (op->kind == OpKind::MulF) ? Opcode::FMul
                                                    : Opcode::FDiv;
      Instruction* r = createBin(k, a, b, op->loc);
      { sir::Value rr = op->result(0); emit(r); bind(rr, r); }
      return Action::kOk;
    }
    case OpKind::MinusF: {
      Value* a = map(op->operand(0));
      if (a == nullptr) { giveUp("MinusF 的操作数缺失", op->loc); return Action::kOk; }
      Instruction* i = createInst(Opcode::FNeg, typePool().f32(), op->loc);
      i->addOperand(a);
      emit(i);                  // `emit` 负责登记（不许再 ownInst 一次）
      bind(op->result(0), i);
      return Action::kOk;
    }
    // ── 比较 ──────────────────────────────────────────────────────────
    case OpKind::Eq: case OpKind::Ne: case OpKind::Lt:
    case OpKind::Le: case OpKind::Gt: case OpKind::Ge: {
      Value* a = map(op->operand(0));
      Value* b = map(op->operand(1));
      if (a == nullptr || b == nullptr) {
        giveUp("比较的操作数缺失", op->loc);
        return Action::kOk;
      }
      const bool isFloat = (a->type() != nullptr && a->type()->isFloat());
      Instruction* i = nullptr;
      if (isFloat) {
        // 谓词映射：`Eq`→`oeq`（**有序**：NaN 比较为假）、`Ne`→`une`（无序：
        //   NaN 时为真）。这是 IRGen 已经定死的语义（结构化层的 `Eq`/`Ne`
        //   在浮点上就是 C 的 `==`/`!=`），平面层只是换个拼写，不改语义。
        const FPred p = (op->kind == OpKind::Eq)   ? FPred::Oeq
                        : (op->kind == OpKind::Ne) ? FPred::Une
                        : (op->kind == OpKind::Lt) ? FPred::Olt
                        : (op->kind == OpKind::Le) ? FPred::Ole
                        : (op->kind == OpKind::Gt) ? FPred::Ogt
                                                   : FPred::Oge;
        i = createFCmp(p, a, b, op->loc);
      } else {
        const IPred p = (op->kind == OpKind::Eq)   ? IPred::Eq
                        : (op->kind == OpKind::Ne) ? IPred::Ne
                        : (op->kind == OpKind::Lt) ? IPred::Slt
                        : (op->kind == OpKind::Le) ? IPred::Sle
                        : (op->kind == OpKind::Gt) ? IPred::Sgt
                                                   : IPred::Sge;
        i = createICmp(p, a, b, op->loc);
      }
      { sir::Value rr = op->result(0); emit(i); bind(rr, i); }
      return Action::kOk;
    }
    // ── 转换 ──────────────────────────────────────────────────────────
    case OpKind::I2F: {
      Value* a = map(op->operand(0));
      if (a == nullptr) { giveUp("I2F 的操作数缺失", op->loc); return Action::kOk; }
      Instruction* r = createCast(Opcode::SIToFP, a, op->loc);
      { sir::Value rr = op->result(0); emit(r); bind(rr, r); }
      return Action::kOk;
    }
    case OpKind::F2I: {
      // ★ 结构化层的 `F2I` 是**饱和转换**（铁律 6：`fptosi(NaN)` 在两个目标上
      //   行为不同 ⇒ 前端必须归一化）。平面层保留**同一语义**，用
      //   `fcmp`+`select` 链展开（对照 `IRGen.cpp` 的 `genSatFptosi`：
      //   那里用嵌套 `if` 是因为结构化层没有 `select` 的降级路径）。
      //   顺序（**必须从最外层往里写**，否则 NaN 会落到错的档）：
      //     select (nan) → 0   [une 对 NaN 为真 ⇒ 优先级最高]
      //     select (tooLow) → INT_MIN
      //     select (tooHigh) → INT_MAX
      //     其余 → fptosi
      Value* a = map(op->operand(0));
      if (a == nullptr) { giveUp("F2I 的操作数缺失", op->loc); return Action::kOk; }
      const Value* hiC = kFlt(f32BitsFor(2147483647), op->loc);
      const Value* loC = kFlt(f32BitsFor(-2147483648), op->loc);
      Instruction* nanv = createFCmp(FPred::Une, a, a, op->loc);      // NaN ⇒ 真
      Instruction* tooHigh = createFCmp(FPred::Ogt, a, const_cast<Value*>(hiC), op->loc);
      Instruction* tooLow = createFCmp(FPred::Olt, a, const_cast<Value*>(loC), op->loc);
      Instruction* conv = createCast(Opcode::FPToSI, a, op->loc);
      emit(nanv);
      emit(tooHigh);
      emit(tooLow);
      emit(conv);
      Value* kMax = kInt(INT32_MAX, op->loc);
      Value* kMin = kInt(INT32_MIN, op->loc);
      Value* kZero = kInt(0, op->loc);
      // 内层：tooHigh ? INT_MAX : conv
      Instruction* selHigh = createSelect(tooHigh, kMax, conv, op->loc);
      emit(selHigh);
      // 中层：tooLow ? INT_MIN : selHigh
      Instruction* selLow = createSelect(tooLow, kMin, selHigh, op->loc);
      emit(selLow);
      // 外层：NaN ? 0 : selLow
      Instruction* selNan = createSelect(nanv, kZero, selLow, op->loc);
      emit(selNan);
      bind(op->result(0), selNan);
      return Action::kOk;
    }
    case OpKind::Sext: {
      Value* a = map(op->operand(0));
      if (a == nullptr) { giveUp("Sext 的操作数缺失", op->loc); return Action::kOk; }
      Instruction* r = createCast(Opcode::SExt, a, op->loc);
      { sir::Value rr = op->result(0); emit(r); bind(rr, r); }
      return Action::kOk;
    }
    case OpKind::Select: {
      Value* c = map(op->operand(0));
      Value* a = map(op->operand(1));
      Value* b = map(op->operand(2));
      if (c == nullptr || a == nullptr || b == nullptr) {
        giveUp("Select 的操作数缺失", op->loc);
        return Action::kOk;
      }
      Instruction* r = createSelect(c, a, b, op->loc);
      { sir::Value rr = op->result(0); emit(r); bind(rr, r); }
      return Action::kOk;
    }
    case OpKind::Phi: {
      // 结构化层**不产出** `PhiOp`（平面层才有 φ）。真遇到就把它的操作数
      //   当"前驱块就是当前块"处理是不对的 ⇒ 报 error（不许静默）。
      giveUp("结构化层出现了 PhiOp（不应发生）", op->loc);
      return Action::kOk;
    }
    // ── 调用 ──────────────────────────────────────────────────────────
    case OpKind::Call: {
      std::vector<Value*> args;
      args.reserve(op->numOperands());
      bool ok = true;
      for (size_t k = 0; k < op->numOperands(); ++k) {
        Value* v = map(op->operand(k));
        if (v == nullptr) { ok = false; break; }
        args.push_back(v);
      }
      if (!ok) { giveUp("Call 的实参缺失", op->loc); return Action::kOk; }
      const sir::Value r = op->result(0);
      const Type* retTy = r != nullptr ? toFlatType(r->type) : nullptr;
      Instruction* i = createCall(op->strAttr(0), retTy, args, op->loc);
      emit(i);
      if (r != nullptr) bind(r, i);
      return Action::kOk;
    }
    default:
      giveUp(std::string("展平遇到未支持的 OpKind：") + sir::opKindName(op->kind), op->loc);
      return Action::kOk;
  }
}

}  // namespace flat
}  // namespace sysy
