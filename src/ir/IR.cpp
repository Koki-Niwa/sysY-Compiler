// ============================================================================
// ir/IR.cpp —— 平面 IR 容器的原语实现（基本块 / 函数 / 模块）
//
//   本文件**不含任何业务语义**：没有指令选择、没有常量折叠、没有化简。
//   唯一的"逻辑"是两个重建点（`rebuildCFG` / `rebuildUseDef`）与统计。
// ============================================================================
#include "ir/Module.h"

#include <algorithm>

namespace sysy {
namespace flat {

// ============================================================================
// BasicBlock
// ============================================================================
void BasicBlock::addInst(Instruction* inst) {
  if (inst == nullptr) return;
  inst->setParent(this);
  insts_.push_back(inst);
}

void BasicBlock::insertInst(size_t i, Instruction* inst) {
  if (inst == nullptr) return;
  inst->setParent(this);
  if (i >= insts_.size()) {
    insts_.push_back(inst);
    return;
  }
  insts_.insert(insts_.begin() + static_cast<long>(i), inst);
}

bool BasicBlock::removeInst(Instruction* inst) {
  for (size_t i = 0; i < insts_.size(); ++i) {
    if (insts_[i] == inst) {
      insts_.erase(insts_.begin() + static_cast<long>(i));
      inst->setParent(nullptr);
      return true;
    }
  }
  return false;
}

void BasicBlock::clearInsts() {
  for (Instruction* i : insts_) i->setParent(nullptr);
  insts_.clear();
}

Instruction* BasicBlock::terminator() const {
  if (insts_.empty()) return nullptr;
  Instruction* last = insts_.back();
  return (last != nullptr && isTerminatorOpcode(last->op())) ? last : nullptr;
}

void BasicBlock::sortPreds() {
  std::sort(preds_.begin(), preds_.end(),
            [](const BasicBlock* a, const BasicBlock* b) { return a->index() < b->index(); });
}

// ============================================================================
// Function
// ============================================================================
void Function::addParam(Instruction* p) {
  if (p == nullptr) return;
  params_.push_back(p);
  if (BasicBlock* e = entry()) e->addParam(p);
}

void Function::addBlock(BasicBlock* b) {
  if (b == nullptr) return;
  b->setIndex(static_cast<uint32_t>(blocks_.size()));
  blocks_.push_back(b);
}

bool Function::removeBlock(BasicBlock* b) {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (blocks_[i] == b) {
      blocks_.erase(blocks_.begin() + static_cast<long>(i));
      reindexBlocks();
      return true;
    }
  }
  return false;
}

void Function::reindexBlocks() {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    blocks_[i]->setIndex(static_cast<uint32_t>(i));
  }
}

size_t Function::instCount() const {
  size_t n = 0;
  for (const BasicBlock* b : blocks_) n += b->size();
  return n;
}

size_t Function::phiCount() const {
  size_t n = 0;
  for (const BasicBlock* b : blocks_) {
    for (const Instruction* i : b->insts()) {
      if (i->op() == Opcode::Phi) ++n;
    }
  }
  return n;
}

// ============================================================================
// Module：工厂
// ============================================================================
// ⚠️ 所有权模型（**唯一一处**必须记住的约定）：
//   工厂**不登记**新指令；登记只能经 `ownInst()`。曾经让 `createInst` 顺手
//   登记，而调用方（`FlattenLower` 的 `emit`）也调 `ownInst` ⇒ 同一条指令
//   进 `insts_` 两次 ⇒ 析构时**二次 delete**（实测直接段错误在
//   `Module::clear()`）。一个看起来"方便"的隐式副作用就是这么变成内存错误的。
Instruction* Module::createInst(Opcode op, const Type* ty, SourceLoc loc) {
  return new Instruction(op, ty, loc);
}

BasicBlock* Module::createBlock(Function* fn, const std::string& name) {
  BasicBlock* b = new BasicBlock(fn, 0);
  blocks_.push_back(b);
  if (!name.empty()) b->setName(name);
  if (fn != nullptr) fn->addBlock(b);
  return b;
}

Function* Module::createFunction(const std::string& name, const Type* retTy) {
  Function* f = new Function(this, name, retTy);
  funcs_.push_back(f);
  return f;
}

GlobalVariable* Module::createGlobal(const std::string& name, const Type* objTy) {
  GlobalVariable* g = new GlobalVariable(name, objTy);
  globals_.push_back(g);
  return g;
}

GlobalAddr* Module::globalAddr(GlobalVariable* gv) {
  if (gv == nullptr) return nullptr;
  const auto it = addrOf_.find(gv);
  if (it != addrOf_.end()) return it->second;
  GlobalAddr* a = new GlobalAddr(gv, gv->addrType());
  globalAddrs_.push_back(a);
  addrOf_[gv] = a;
  return a;
}

Function* Module::findFunction(const std::string& name) const {
  for (Function* f : funcs_) {
    if (f->name() == name) return f;
  }
  return nullptr;
}

GlobalVariable* Module::findGlobal(const std::string& name) const {
  for (GlobalVariable* g : globals_) {
    if (g->name() == name) return g;
  }
  return nullptr;
}

// ── 常量：**按位模式去重**（浮点比位，不比 `==`）───────────────────────────
Constant* Module::getIntConst(const Type* ty, int64_t v) {
  const std::string key = typeText(ty) + ":" + std::to_string(v);
  const auto it = intConsts_.find(key);
  if (it != intConsts_.end()) return it->second;
  Constant* c = new Constant(ty, v);
  consts_.push_back(c);
  intConsts_[key] = c;
  return c;
}

Constant* Module::getFloatConst(uint32_t bits) {
  const auto it = floatConsts_.find(bits);
  if (it != floatConsts_.end()) return it->second;
  Constant* c = new Constant(typePool().f32());
  c->setFBits(bits);
  consts_.push_back(c);
  floatConsts_[bits] = c;
  return c;
}

// ============================================================================
// Module：两个重建点
// ============================================================================
void Module::rebuildCFG() {
  for (Function* f : funcs_) {
    for (BasicBlock* b : f->blocks()) b->clearPreds();
    for (BasicBlock* b : f->blocks()) {
      Instruction* t = b->terminator();
      if (t == nullptr) continue;
      for (size_t i = 0; i < t->numSuccs(); ++i) {
        BasicBlock* s = t->succ(i);
        if (s == nullptr) continue;
        // 去重：`br` 的两个目标**可以是同一个块**（`x ? L1 : L1`），
        //   那在前驱表里只算**一条**边（φ 也只该有一个入值）。
        //   不去重会让 `preds()` 多一项 ⇒ 与 φ 的入值个数对不上（实测 V3）。
        bool dup = false;
        for (size_t k = 0; k < s->numPreds(); ++k) {
          if (s->pred(k) == b) { dup = true; break; }
        }
        if (!dup) s->addPred(b);
      }
    }
    for (BasicBlock* b : f->blocks()) b->sortPreds();
  }
}

void Module::rebuildUseDef() {
  for (Instruction* i : insts_) i->clearUsers();
  // ⚠️ 本函数**只遍历 `insts_`** ⇒ 漏登记的指令在这里不存在。每个"造指令"的
  //   地方都必须调 `ownInst`（`Module::ownInst` 是幂等的，见它的说明）。
  for (Instruction* i : insts_) {
    for (size_t k = 0; k < i->numOperands(); ++k) {
      Value* v = i->operand(k);
      if (v == nullptr) continue;
      if (v->isInst()) static_cast<Instruction*>(v)->addUser(i);
    }
  }
}

// ============================================================================
// 统计
// ============================================================================
Module::Stats Module::stats() const {
  Stats s;
  s.funcs = funcs_.size();
  s.globals = globals_.size();
  for (const Function* f : funcs_) {
    if (f->isDeclaration()) continue;
    s.blocks += f->blockCount();
    s.insts += f->instCount();
    s.phis += f->phiCount();
    s.params += f->numParams();
  }
  return s;
}

std::string Module::formatStats() const {
  const Stats s = stats();
  std::string out;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "flat-stats funcs=%zu blocks=%zu insts=%zu phis=%zu params=%zu globals=%zu\n",
                s.funcs, s.blocks, s.insts, s.phis, s.params, s.globals);
  out += buf;
  for (const Function* f : funcs_) {
    if (f->isDeclaration()) continue;
    std::snprintf(buf, sizeof(buf), "  flat-func %s blocks=%zu insts=%zu phis=%zu\n",
                  f->name().c_str(), f->blockCount(), f->instCount(), f->phiCount());
    out += buf;
  }
  return out;
}

// ============================================================================
// 释放（**迭代**）
// ============================================================================
void Module::clear() {
  // ⚠️ 这里对**每一条指令只 delete 一次**是硬前提：重复登记 = 二次 delete
  //   （实测段错误）。所以"谁造谁登记"必须唯一 —— 见 `Module::ownInst` 的说明。
  // ⚠️ 每条指令只能进 `insts_` **一次**（重复登记 = 二次 delete = 段错误）。
  //   最隐蔽的一种重复来源：`emit(createBin(Sub, kInt(0, loc), a, loc))` ——
  //   `kInt` 造的常量**先**被登记（它是操作数），随后整个 `sub` 也被登记，
  //   而 `sub` 的构造又把那个常量塞进自己的 `insts_` 引用链……真正会炸的是
  //   "同一个对象既当操作数被 `emit`、又当结果被 `emit`"。
  //   ⇒ 纪律：**`emit()` 只能用于"其结果要被使用"的那一条指令，
  //     操作数位置的东西一律不许再 `emit`**（它们由使用者带着登记）。
  // ⚠️ 先**摘出**块（`removeInst` 会把 `parent` 置 nullptr），再 delete：
  //   否则块里留着悬垂指针，块析构时 `clearInsts()` 会去写已经释放的对象
  //   （ASAN 实测：`Module::clear()` 里 heap-use-after-free）。
  for (Instruction* i : insts_) {
    if (i == nullptr) continue;
    if (BasicBlock* b = i->parent()) b->removeInst(i);
    delete i;
  }
  insts_.clear();
  for (BasicBlock* b : blocks_) delete b;
  blocks_.clear();
  for (Function* f : funcs_) delete f;
  funcs_.clear();
  for (GlobalVariable* g : globals_) delete g;
  globals_.clear();
  for (Constant* c : consts_) delete c;
  consts_.clear();
  for (GlobalAddr* a : globalAddrs_) delete a;
  globalAddrs_.clear();
  ownedSet_.clear();
  intConsts_.clear();
  floatConsts_.clear();
  addrOf_.clear();
}

}  // namespace flat
}  // namespace sysy
