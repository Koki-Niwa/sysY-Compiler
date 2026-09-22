// ============================================================================
// structured/FlattenCFG.cpp —— FlattenCFG 的**块骨架**实现
//
//   契约、φ 放置判据、临界边拆分的**完整说明在 `FlattenCFG.h` 的文件头**
//   （先读那里）。单条 Op 的降级在 `FlattenLower.cpp`。
//
// ── 一个必须小心的次序 ───────────────────────────────────────────────────
//   块的 **φ 必须排在块的最前面**（后端契约 4）。所以"建块"与"决定它的环境"
//   是分开的两步：先 `newBlock()`，再用 `enterJoin()`（它立刻在块开头发 φ），
//   然后才处理块里的其余内容。顺序反了 φ 就会跑到指令后面。
// ============================================================================
#include <algorithm>

#include "structured/FlattenInternal.h"

namespace sysy {
namespace flat {

// ============================================================================
// 类型映射（同一个形状、同一个类型池；见 `ir/Type.h` 的文件头）
// ============================================================================
const Type* toFlatType(const sir::Type* t) {
  if (t == nullptr) return nullptr;
  switch (t->kind) {
    case sir::TypeKind::Void: return typePool().voidTy();
    case sir::TypeKind::I32:  return typePool().i32();
    case sir::TypeKind::I64:  return typePool().i64();
    case sir::TypeKind::F32:  return typePool().f32();
    case sir::TypeKind::Ptr:  return typePool().ptrTo(toFlatType(t->elem));
    case sir::TypeKind::Array: {
      const Type* e = toFlatType(t->elem);
      return e == nullptr ? nullptr : typePool().arrayOf(e, t->len);
    }
  }
  return nullptr;
}

// ============================================================================
// 入口：遍历 ModuleOp
// ============================================================================
Module* flattenModule(const sir::Op* smod, Module& out, DiagnosticEngine& diags) {
  if (smod == nullptr) return nullptr;
  if (smod->kind != sir::OpKind::Module) {
    diags.report(DiagLevel::Error, smod->loc, "E-FLATGEN",
                 "flattenModule 的输入不是 ModuleOp");
    return nullptr;
  }
  const sir::Region* modRegion = smod->numRegions() > 0 ? smod->region(0) : nullptr;
  if (modRegion == nullptr) {
    diags.report(DiagLevel::Error, smod->loc, "E-FLATGEN", "ModuleOp 没有 Region");
    return nullptr;
  }
  out.setSourceName(smod->strAttr(0));

  // ① 全局变量（顺序 = 结构化层的声明顺序）
  for (const Op* op : modRegion->ops()) {
    if (op == nullptr || op->kind != sir::OpKind::GlobalVar) continue;
    const Type* objTy = toFlatType(op->typeAttr(1));
    if (objTy == nullptr) {
      diags.report(DiagLevel::Error, op->loc, "E-FLATGEN",
                   "全局对象 `" + op->strAttr(0) + "` 的类型无法映射");
      continue;
    }
    GlobalVariable* g = out.createGlobal(op->strAttr(0), objTy);
    g->setLoc(op->loc);
    // `:init` 属性是 `"zero"` 或 `"data"` + 数据表（与 S05 的 dump 一致）。
    if (op->strAttr(2) != "data") {
      g->setZero();
    } else {
      GlobalVariable::Data data;
      for (size_t i = 3; i < op->attrs().size(); ++i) {
        const sir::Attr& a = op->attrs()[i];
        if (a.kind != sir::Attr::Kind::Data) continue;
        for (const auto& kv : a.data) data.emplace_back(kv.first, kv.second);
      }
      g->setInitData(std::move(data));
    }
    out.globalAddr(g);   // 预建地址值（顺序 = 全局声明顺序）
  }

  // ② 模块级 Op（`GetGlobal`）与 ③ 全部函数：**共用同一个 FlatBuilder**
  //    （同一个 `vals_`：值的绑定跨"模块级 → 函数级"必须连续）。
  {
    FlatBuilder b(out, diags);
    b.lowerModuleRegion(modRegion);
    for (const Op* op : modRegion->ops()) {
      if (op == nullptr || op->kind != sir::OpKind::Func) continue;
      if (b.run(op) == nullptr) return nullptr;
    }
  }

  out.rebuildCFG();
  out.rebuildUseDef();
  return &out;
}

// ============================================================================
// 基础设施
// ============================================================================
// 【后置】模块级 Op 的降级（目前只有 `GetGlobal`：把"全局的地址"这个值绑好）。
//   `GlobalVar` 在 ① 里已经建过对象，这里不需要再做（跳过即可）。
void FlatBuilder::lowerModuleRegion(const Region* modRegion) {
  if (modRegion == nullptr) return;
  for (Op* op : modRegion->ops()) {
    if (op == nullptr) continue;
    if (op->kind == OpKind::GlobalVar) continue;   // ① 里已经建过对象
    if (op->kind == OpKind::Func) continue;        // 由 `run()` 单独展平
    if (op->kind == OpKind::GetGlobal) {
      const std::string& gn = op->strAttr(0);
      GlobalVariable* g = m_.findGlobal(gn);
      if (g == nullptr) {
        giveUp("GetGlobal 引用了不存在的全局 `" + gn + "`", op->loc);
        continue;
      }
      // 池化：同一个全局在所有位置共用**一个**地址值（dump 才稳定）
      auto it = globalPool_.find(gn);
      if (it == globalPool_.end()) {
        it = globalPool_.emplace(gn, m_.globalAddr(g)).first;
      }
      bind(op->result(0), it->second);
      continue;
    }
    giveUp(std::string("模块级出现未支持的 Op：") + sir::opKindName(op->kind), op->loc);
  }
}

void FlatBuilder::giveUp(const std::string& msg, SourceLoc loc) {
  diag_.report(DiagLevel::Error, loc, "E-FLATGEN", msg);
  failed_ = true;
}

BasicBlock* FlatBuilder::newBlock(const std::string& name) {
  if (blocks_.size() >= kMaxBlocksPerFunc) {
    if (!overflowed_) {
      overflowed_ = true;
      giveUp("函数 " + f_->name() + " 的基本块数超过 " +
                 std::to_string(kMaxBlocksPerFunc) + "（展平放弃）",
             SourceLoc(0, 0));
    }
    // 仍然返回一个块：调用方不会解引用 nullptr，而失败标志会让外层放弃产物
  }
  BasicBlock* b = m_.createBlock(f_, name);
  blocks_.push_back(b);
  return b;
}

Instruction* FlatBuilder::emit(Instruction* i) {
  if (++instCount_ > kMaxInstsPerFunc) {
    if (!overflowed_) {
      overflowed_ = true;
      giveUp("函数 " + f_->name() + " 的指令数超过 " +
                 std::to_string(kMaxInstsPerFunc) + "（展平放弃）",
             SourceLoc(0, 0));
    }
    return i;
  }
  ownInst(i);
  cur_->addInst(i);
  return i;
}

void FlatBuilder::enterBlock(BasicBlock* b, const Env& env) {
  cur_ = b;
  env_ = env;
}

Value* FlatBuilder::map(sir::Value v) const {
  if (v == nullptr) return nullptr;
  const auto it = vals_.find(v);
  if (it != vals_.end()) return it->second;
  const auto p = params_.find(v);
  if (p != params_.end()) return p->second;
  return nullptr;
}

void FlatBuilder::bind(sir::Value v, Value* fv) {
  if (v != nullptr && fv != nullptr) vals_[v] = fv;
}

// 【后置】造一个**未登记**的整型常量。
//   ⚠️ 常量也是指令，而"登记"只能做一次：这里**不登记**，
//   由调用方决定（当操作数用 ⇒ 由 `emit` 的那条指令带上；自己单独出现在
//   某个块里 ⇒ 调用方显式 `ownInst`）。第一版在这里顺手登记，
//   而调用方又 `emit(createBin(…, kInt(…)))` ⇒ 同一条常量进 `insts_` 两次
//   ⇒ 析构时二次 delete（实测：全量扫描里 39 个文件段错误）。
Value* FlatBuilder::constInt(int64_t v, const Type* ty, SourceLoc loc) {
  // 池化（键 = 类型文本 + 值）：同一份 IR 里同值常量**只定义一次**
  const std::string key = f_->name() + ":i:" + typeText(ty) + ":" + std::to_string(v);
  const auto it = constPool_.find(key);
  if (it != constPool_.end()) return it->second;
  Instruction* c = m_.createInst(Opcode::ConstantInt, ty, loc);
  c->setIntBits(v);
  constPool_[key] = c;
  return c;
}
Value* FlatBuilder::kInt(int32_t v, SourceLoc loc) {
  return constInt(v, typePool().i32(), loc);
}
Value* FlatBuilder::kI64(int64_t v, SourceLoc loc) {
  return constInt(v, typePool().i64(), loc);
}
Value* FlatBuilder::kFlt(uint32_t bits, SourceLoc loc) {
  // 池化：浮点常数按**位模式**去重（`-0.0` 与 `+0.0` 位模式不同 ⇒ 不合并）
  const std::string key = f_->name() + ":f:" + std::to_string(bits);
  const auto it = constPool_.find(key);
  if (it != constPool_.end()) return it->second;
  Instruction* c = m_.createInst(Opcode::ConstantFP, typePool().f32(), loc);
  c->setFloatBits(bits);
  constPool_[key] = c;
  return c;   // 未登记（见 constInt 的说明）
}

const Type* FlatBuilder::slotType(Value* slot) const {
  const Type* pt = slot != nullptr ? slot->type() : nullptr;
  return (pt != nullptr && pt->isPtr()) ? pt->elem : typePool().i32();
}

Value* FlatBuilder::zeroOfSlot(Value* slot, SourceLoc loc) {
  const Type* t = slotType(slot);
  Instruction* c = nullptr;
  if (t != nullptr && t->isFloat()) {
    c = static_cast<Instruction*>(kFlt(0u, loc));
  } else {
    c = static_cast<Instruction*>(constInt(0, t != nullptr ? t : typePool().i32(), loc));
  }
  ownInst(c);   // φ 的入值可能只在这里出现一次 ⇒ 由本函数负责登记
  return c;
}

void FlatBuilder::setSlot(Value* slot, Value* v) {
  if (slot == nullptr) return;
  env_[slot] = v;
}

// 【后置】`slot` 对应的结构化槽是否被读过（**判据 2 的判定**）。
bool FlatBuilder::isReadSlot(Value* slot) const {
  const auto it = sirOfSlot_.find(slot);
  return it != sirOfSlot_.end() && readSlotsSir_.count(it->second) > 0;
}

void FlatBuilder::collectReadSlots(Region* region) {
  if (region == nullptr) return;
  for (Op* op : region->ops()) {
    if (op == nullptr) continue;
    // `Load` 的指针操作数就是"被读的槽"（结构化层的 Alloca 结果）
    if (op->kind == OpKind::Load && op->numOperands() > 0 &&
        op->operand(0) != nullptr) {
      readSlotsSir_.insert(op->operand(0));
    }
    for (size_t k = 0; k < op->numRegions(); ++k) collectReadSlots(op->region(k));
  }
}

void FlatBuilder::collectStoredSlots(Region* region,
                                     std::unordered_set<Value*>& out) const {
  if (region == nullptr) return;
  for (Op* op : region->ops()) {
    if (op == nullptr) continue;
    if (op->kind == OpKind::Store && op->numOperands() > 1) {
      Value* slot = map(op->operand(1));
      if (slot != nullptr) out.insert(slot);
    }
    // 递归：`if` 分支里的写也算"体里被写"（φ 的候选要覆盖它）
    for (size_t k = 0; k < op->numRegions(); ++k) collectStoredSlots(op->region(k), out);
  }
}

// ============================================================================
// ★ 汇合点的 φ 放置（判据见 FlattenCFG.h 文件头）
// 【前置】b 是新块；`inEnvs` 按**边序**给出每条入边的槽 → 值；
//         `edgeBlocks` 与 `inEnvs` **一一对应**（φ 的入值键）。
// 【后置】b 的开头放好 φ（只在"各入边值不全相同"时放），
//         `env_` = b 的块内环境初值（含 φ 的结果）。
// ============================================================================
void FlatBuilder::enterJoin(BasicBlock* b, std::vector<Env> inEnvs, SourceLoc loc,
                            const std::vector<BasicBlock*>& edgeBlocks) {
  if (inEnvs.size() != edgeBlocks.size()) {
    giveUp("汇合块的入边数与入值块数不一致（内部错误）", loc);
    return;
  }
  cur_ = b;
  env_ = Env();
  // 收集"至少一条入边上出现过"的槽（按首次出现顺序 ⇒ 确定性）
  std::vector<Value*> slots;
  std::unordered_set<Value*> seen;
  for (const Env& e : inEnvs) {
    for (const auto& kv : e) {
      if (seen.insert(kv.first).second) slots.push_back(kv.first);
    }
  }
  for (Value* slot : slots) {
    std::vector<Value*> vv;
    vv.reserve(inEnvs.size());
    bool allSame = true;
    bool firstSet = false;
    Value* first = nullptr;
    for (const Env& e : inEnvs) {
      const auto it = e.find(slot);
      Value* v = (it == e.end()) ? nullptr : it->second;
      vv.push_back(v);
      if (!firstSet) { first = v; firstSet = true; }
      else if (v != first) { allSame = false; }
    }
    if (allSame && first != nullptr) {
      env_[slot] = first;      // 判据 2 的"不多放"：全都一样就不放 φ
      continue;
    }
    std::vector<Value*> vals;
    vals.reserve(vv.size());
    for (Value* v : vv) vals.push_back(v != nullptr ? v : zeroOfSlot(slot, loc));
    Instruction* phi = createPhi(slotType(slot), vals, edgeBlocks, loc);
    ownInst(phi);
    cur_->addInst(phi);        // ★ 必须在块内其它指令之前（这里是块首）
    env_[slot] = phi;
  }
}

// ============================================================================
// ★ 临界边拆分（prompt §二.3）
//   一条边 `(src, dst)` 是**临界边** ⟺ `outdeg(src) > 1 && indeg(dst) > 1`。
//   不拆的话 φ 无法表达"这次是从哪条边来的" ⇒ 结果偶尔错。
//   做法：插入一个**空的中转块**（只有 `br dst`），并把 φ 的入值键从 src
//   改到中转块。中转块**没有 `@line`**（它不对应任何源码位置）。
//   ⚠️ 必须**在所有 φ 都放好之后**做（否则新插的中转块不会出现在 φ 的入值里）。
// ============================================================================
void FlatBuilder::splitCriticalEdges() {
  for (size_t bi = 0; bi < blocks_.size(); ++bi) {
    BasicBlock* src = blocks_[bi];
    Instruction* t = src->terminator();
    if (t == nullptr || t->op() != Opcode::Br || t->numSuccs() < 2) continue;
    for (size_t si = 0; si < t->numSuccs(); ++si) {
      BasicBlock* dst = t->succ(si);
      if (dst == nullptr || dst->numPreds() < 2) continue;
      BasicBlock* mid = newBlock("crit");
      Instruction* br = createBr(dst, SourceLoc());
      ownInst(br);
      mid->addInst(br);
      t->setSucc(si, mid);                       // ① 源块的这条边改指中转块
      for (size_t ii = 0; ii < dst->size(); ++ii) {   // ② φ 的入值键改到中转块
        Instruction* phi = dst->at(ii);
        if (phi->op() != Opcode::Phi) break;     // φ 只在块首连续排列
        for (size_t k = 0; k < phi->numSuccs(); ++k) {
          if (phi->succ(k) == src) phi->setSucc(k, mid);
        }
      }
    }
  }
}

// ============================================================================
// 主遍历（显式帧栈）
// ============================================================================
Function* FlatBuilder::run(const Op* fn) {
  // ★★ 每个函数都必须**清空上一轮的全部状态** ★★
  //   `flattenModule` 用**同一个 `FlatBuilder`** 展平同一模块的多个函数
  //   （那是必要的：模块级的全局地址绑定要跨函数可见）。但下面这些是
  //   **按函数**的：块表、块号计数器、块内环境、被读过的槽、值映射…
  //   ⚠️ 不清空的后果（实测 340 个多函数文件）：第二个函数的 `newBlock`
  //   沿用第一个函数留下的 `blocks_.size()` 当块号 ⇒ **块号重复**
  //   （`L0` 出现两次）⇒ dump 不可读回、轨 A/轨 E 全线失败。
  //   ⚠️ 但 `vals_` **不能**在这里清：里面同时装着**模块级**的绑定
  //   （`GetGlobal` 的地址值，见 `lowerModuleRegion`），清掉它会让第二个
  //   函数里的 `store … @g` 找不到操作数（实测："Store 的操作数/类型缺失"）。
  //   ⇒ 分两类：**模块级**的（`vals_`/`constPool_`/`globalPool_`）保留；
  //     **函数级**的（块表/环境/被读槽/计数器）每个函数重置。
  //     用 `started_` 区分"第一个函数"与"后续函数"，避免误清模块级状态。
  if (started_) {
    env_.clear();
    readSlotsSir_.clear();
    sirOfSlot_.clear();
    params_.clear();
    breaks_.clear();
  }
  started_ = true;
  blocks_.clear();
  instCount_ = 0;
  failed_ = false;
  overflowed_ = false;
  const std::string name = fn->strAttr(0);
  f_ = m_.createFunction(name, toFlatType(fn->typeAttr(1)));
  f_->setLoc(fn->loc);
  cur_ = newBlock("entry");
  enterBlock(cur_, Env());

  Region* body = fn->numRegions() > 0 ? fn->region(0) : nullptr;
  if (body == nullptr) return f_;

  // ① 形参：结构化层是体开头的 `GetArg`，平面层是入口块的形参。
  //    ⚠️ 必须**先**扫 `GetArg`（后面 `collectReadSlots` 要靠它们映射槽）。
  //   ⚠️ `GetArg` **不是**连续排在体开头的：IRGen 的形态是
  //      `GetArg a; Alloca; Store a; GetArg b; Alloca; Store b; …`（实测）。
  //      第一版写成"遇到第一个非 GetArg 就 break" ⇒ 第二个形参开始全部丢失，
  //      下游 `map()` 返回 nullptr、报"Store 的操作数/类型缺失"。
  //      ⇒ 扫**整个体**，按 `intAttr(0)` 的顺序收集（只认下标连续的）。
  for (Op* op : body->ops()) {
    if (op == nullptr || op->kind != OpKind::GetArg) continue;
    const int64_t idx = op->intAttr(0);
    const Type* pty = toFlatType(op->result(0) != nullptr ? op->result(0)->type : nullptr);
    if (idx < 0 || static_cast<size_t>(idx) != f_->numParams()) {
      giveUp("GetArg 的下标不连续（" + std::to_string(idx) + "），无法展平", op->loc);
      break;
    }
    Instruction* p = createParam(pty, op->loc);
    ownInst(p);
    f_->addParam(p);
    if (op->result(0) != nullptr) params_[op->result(0)] = p;
  }
  // ② 先扫"被读过的槽"（判据 2 的输入）。**必须在展平之前扫完**：
  //   "先读后写"的槽也要进环境，否则那次 load 不走环境，φ 就不会被放置。
  collectReadSlots(body);
  // ③ alloca：结构化层已由 `AllocaHoist` 提到体最前（不变量 2），
  //   平面层保持同样的相对顺序（后端照这个顺序一次性布局栈帧）。
  //   它们由主循环正常处理（`lower` 把 `Alloca` 发在当前块里）。

  walk(body);
  if (failed_) return f_;
  if (!hasTerm(cur_) && !hasTerm(f_->entry())) {
    // 函数体没有任何终结符（畸形 IR）：补 `unreachable` 而不是静默产出空块
    emit(createUnreachable(SourceLoc()));
  }
  splitCriticalEdges();
  // ★ 块号唯一性自查：`L<n>` 是 dump 的引用键，**重复索引会让 dump 不可读回**
  //   （读回器判"重复的标签"）。blocks_ 的 index 只在 `addBlock`/`reindexBlocks`
  //   里设置，理论上唯一 —— 这里把它变成**可执行的判据**，出问题立刻报出来
  //   而不是留下一个"读不回来"的 dump。
  {
    std::unordered_set<uint32_t> seenIdx;
    for (BasicBlock* b : blocks_) {
      if (b == nullptr) continue;
      if (!seenIdx.insert(b->index()).second) {
        giveUp("块号重复：L" + std::to_string(b->index()) + "（展平内部错误）", SourceLoc(0, 0));
        break;
      }
    }
  }
  return f_;
}

void FlatBuilder::walk(Region* body) {
  Frame top;
  top.kind = FrameKind::Region;
  top.region = body;
  top.i = 0;
  top.startBlock = cur_;
  top.cont = nullptr;
  stack_.clear();
  stack_.push_back(top);

  while (!stack_.empty()) {
    if (stack_.back().kind == FrameKind::Finish) {
      std::function<void()> f = stack_.back().finish;
      stack_.pop_back();
      if (f) f();
      if (failed_) return;
      continue;
    }
    if (stack_.back().region == nullptr || stack_.back().i >= stack_.back().region->size()) {
      Frame& fr = stack_.back();
      // ★ 补跳转发到 **`cur_`**（= 这个 Region 真正走到的那个块）。
      //   ⚠️ 曾经写的是"发到 `startBlock`"（这个 Region 进入时的块）——
      //   那是错的：Region 里若有**嵌套的控制流容器**，它会在最后留下自己的
      //   汇合块，`cur_` 已经变成那个块了。把跳转发回起始块会造成
      //   **两个块各缺/各多一条终结符**（实测症状：`38_op_priority4.sy` 的
      //   条件指令出现在错误的块里、`br` 落到上一轮的旧当前块上，
      //   最终 `V4`/`V5` 一起报红）。
      if (fr.cont != nullptr && !hasTerm(cur_)) {
        emit(createBr(fr.cont, SourceLoc()));
      }
      const uint32_t seq = fr.envSeq;
      const int slot = fr.finishSlot;
      const int beSlot = fr.backedgeSlot;
      BasicBlock* eb = cur_;
      BasicBlock* se = fr.streamEnd;
      stack_.pop_back();
      // 记下"这个 Region 走完时的环境"与**出口块**（容器的收尾帧要用）。
      //   ⚠️ 必须在 `pop_back` **之后**写 `frameEnvs_[seq]`：`fr` 那时已失效。
      if (seq != kNoEnv) frameEnvs_[seq] = env_;
      if (slot >= 0) {
        frameExitBlocks_[static_cast<size_t>(slot)] = eb;
        frameExitEnvs_[static_cast<size_t>(slot)] = env_;
      }
      // 回边/汇合的源块：优先用"指令流结束的块"（`Yield` 记下的），
      //   没有 `Yield` 时才退回 `cur_`（见 Frame::streamEnd 的说明）。
      if (beSlot >= 0) {
        frameExitBlocks_[static_cast<size_t>(beSlot)] = (se != nullptr ? se : eb);
      }
      continue;
    }
    Frame& fr = stack_.back();
    Op* op = fr.region->at(fr.i);
    ++fr.i;
    if (op == nullptr) continue;
    // 控制流容器先处理（它们要压帧）
    if (op->kind == OpKind::If) {
      if (lowerIf(op, fr) == Action::kStop) return;
      continue;
    }
    if (op->kind == OpKind::While) {
      if (lowerWhile(op, fr) == Action::kStop) return;
      continue;
    }
    if (op->kind == OpKind::For) {
      if (lowerFor(op, fr) == Action::kStop) return;
      continue;
    }
    if (sir::isTerminator(op->kind)) {
      (void)lowerTerminator(op, fr);
      continue;
    }
    if (lower(op) == Action::kStop) return;
  }
}

}  // namespace flat
}  // namespace sysy
