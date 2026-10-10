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
  if (inst == nullptr ||
      inst->lifecycle() == InstructionLifecycle::Tombstone) return;
  inst->setParent(this);
  inst->setLifecycle(InstructionLifecycle::Live);
  insts_.push_back(inst);
  if (parent_ != nullptr && parent_->parent() != nullptr)
    parent_->parent()->rawMarkLive(inst);
}

void BasicBlock::insertInst(size_t i, Instruction* inst) {
  if (inst == nullptr ||
      inst->lifecycle() == InstructionLifecycle::Tombstone) return;
  inst->setParent(this);
  inst->setLifecycle(InstructionLifecycle::Live);
  if (i >= insts_.size()) insts_.push_back(inst);
  else insts_.insert(insts_.begin() + static_cast<long>(i), inst);
  if (parent_ != nullptr && parent_->parent() != nullptr)
    parent_->parent()->rawMarkLive(inst);
}

bool BasicBlock::removeInst(Instruction* inst) {
  for (size_t i = 0; i < insts_.size(); ++i) {
    if (insts_[i] == inst) {
      insts_.erase(insts_.begin() + static_cast<long>(i));
      inst->setParent(nullptr);
      if (parent_ != nullptr && parent_->parent() != nullptr)
        parent_->parent()->rawMarkDetached(inst);
      inst->setLifecycle(InstructionLifecycle::Detached);
      return true;
    }
  }
  return false;
}

void BasicBlock::clearInsts() {
  for (Instruction* i : insts_) {
    i->setParent(nullptr);
    i->setLifecycle(InstructionLifecycle::Detached);
    if (parent_ != nullptr && parent_->parent() != nullptr)
      parent_->parent()->rawMarkDetached(i);
  }
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
void Function::addParam(Argument* p) {
  if (p == nullptr || p->ownerModule() != parent_ || p->owner() != nullptr)
    return;
  p->registerOwner(this, params_.size());
  params_.push_back(p);
  if (BasicBlock* e = entry()) e->addParam(p);
}

bool Function::addBlock(BasicBlock* b) {
  if (b == nullptr || parent_ == nullptr || b->parent() != this ||
      b->ownerModule() != parent_ ||
      std::find(blocks_.begin(), blocks_.end(), b) != blocks_.end())
    return false;
  b->setIndex(static_cast<uint32_t>(blocks_.size()));
  blocks_.push_back(b);
  return true;
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

Argument* Module::createArgument(const Type* ty, SourceLoc loc) {
  Argument* arg = new Argument(ty, loc);
  arg->ownerModule_ = this;
  args_.push_back(arg);
  return arg;
}

BlockAddr* Module::createBlockAddr(const Type* ty, BasicBlock* block) {
  if (!ownsBlock(block) || block->parent() == nullptr ||
      block->parent()->parent() != this)
    return nullptr;
  BlockAddr* value = new BlockAddr(ty);
  value->ownerModule_ = this;
  value->block = block;
  blockAddrs_.push_back(value);
  return value;
}

BasicBlock* Module::createBlock(Function* fn, const std::string& name) {
  if (fn == nullptr ||
      std::find(funcs_.begin(), funcs_.end(), fn) == funcs_.end() ||
      fn->parent() != this)
    return nullptr;
  BasicBlock* b = new BasicBlock(fn, 0);
  b->ownerModule_ = this;
  if (!fn->addBlock(b)) {
    delete b;
    return nullptr;
  }
  if (!name.empty()) b->setName(name);
  blocks_.push_back(b);
  ownedBlockSet_.insert(b);
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
  if (gv == nullptr ||
      std::find(globals_.begin(), globals_.end(), gv) == globals_.end())
    return nullptr;
  const auto it = addrOf_.find(gv);
  if (it != addrOf_.end()) return it->second;
  GlobalAddr* a = new GlobalAddr(gv, gv->addrType());
  a->ownerModule_ = this;
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

// Canonical scalar constants: type pointer and raw bits, including NaN payloads.
Constant* Module::getIntConst(const Type* ty, int64_t v) {
  if (ty == nullptr || (ty->kind != TypeKind::I32 && ty->kind != TypeKind::I64))
    return nullptr;
  const uint64_t bits = ty->kind == TypeKind::I32
      ? static_cast<uint32_t>(v) : static_cast<uint64_t>(v);
  const ConstantKey key{ty, bits};
  auto it = constants_.find(key);
  if (it != constants_.end()) return it->second;
  Constant* c = new Constant(ty, bits);
  c->ownerModule_ = this;
  consts_.push_back(c);
  constants_.emplace(key, c);
  return c;
}

Constant* Module::getFloatConst(uint32_t bits) {
  const ConstantKey key{typePool().f32(), bits};
  auto it = constants_.find(key);
  if (it != constants_.end()) return it->second;
  Constant* c = new Constant(key.type, bits);
  c->ownerModule_ = this;
  consts_.push_back(c);
  constants_.emplace(key, c);
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
  // Repair/debug fallback. All operand primitives already maintain these edges.
  std::unordered_set<Value*> values;
  for (Instruction* inst : insts_) {
    values.insert(inst);
    for (Value* value : inst->operands()) if (value != nullptr) values.insert(value);
  }
  for (Argument* arg : args_) values.insert(arg);
  for (Constant* constant : consts_) values.insert(constant);
  for (GlobalAddr* addr : globalAddrs_) values.insert(addr);
  for (BlockAddr* addr : blockAddrs_) values.insert(addr);
  // Operand-reachable foreign BlockAddr values are also visited.
  for (Value* value : values) value->clearUses();
  for (Instruction* inst : insts_) {
    for (size_t index = 0; index < inst->numOperands(); ++index) {
      if (Value* value = inst->operand(index)) value->addUse(inst, index);
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
  // F00: ownInst owns actual Instructions; canonical Constants and Arguments
  // have independent Module ownership and are never stored in insts_.
  // ⚠️ 先**摘出**块（`removeInst` 会把 `parent` 置 nullptr），再 delete：
  //   否则块里留着悬垂指针，块析构时 `clearInsts()` 会去写已经释放的对象
  //   （ASAN 实测：`Module::clear()` 里 heap-use-after-free）。
  // Detach every edge before freeing any potential operand Value.
  for (Instruction* i : insts_) if (i != nullptr) i->clearOperands();
  for (Instruction* i : insts_) {
    if (i == nullptr) continue;
    if (BasicBlock* b = i->parent()) b->removeInst(i);
    delete i;
  }
  insts_.clear();
  for (BasicBlock* b : blocks_) delete b;
  blocks_.clear();
  ownedBlockSet_.clear();
  for (Function* f : funcs_) delete f;
  funcs_.clear();
  for (GlobalVariable* g : globals_) delete g;
  globals_.clear();
  for (Constant* c : consts_) delete c;
  consts_.clear();
  for (Argument* a : args_) delete a;
  args_.clear();
  for (GlobalAddr* a : globalAddrs_) delete a;
  globalAddrs_.clear();
  for (BlockAddr* a : blockAddrs_) delete a;
  blockAddrs_.clear();
  ownedSet_.clear();
  liveSet_.clear();
  // Lifetime reset is not a tracked optimizer mutation; a revision never rolls back.
  constants_.clear();
  addrOf_.clear();
}

}  // namespace flat
}  // namespace sysy
