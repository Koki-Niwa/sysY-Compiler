// ============================================================================
// ir/BasicBlock.h —— 基本块
//
//   基本块 = **指令序列 + 恰好一个终结符**（prompt §三.3）。
//   `br`/`br cond`/`ret`/`unreachable` 是四种终结符形态；
//   `call` + `ret`（即 `llvm.memcpy` 之后紧接 return）**不需要**特殊处理：
//   终结符仍然是那条 `ret`（`call` 不是终结符）。
//
// ── 所有权与指针稳定性 ────────────────────────────────────────────────────
//   * 指令、基本块、函数、全局对象**全部由 `Module` 持有**
//     （`Module::createInst` / `createBlock` / …），析构是**迭代**的
//     ⇒ 4000 层深的 IR 不会在析构时爆栈（S02 的 `destroyTree` 教训）。
//   * `Value*` / `BasicBlock*` 在 Module 生命期内**永不失效** —— 这是
//     平面层所有 pass 能安全持有指针的前提（与 S05 的 Arena 同一条理由）。
//
// ── F00 operand/use ─────────────────────────────────────────────────────
//   Instruction 的 operands() 与任意 Value 的 uses() 双向精确对应。
//   operand primitive 增量维护每一个 operandIndex 的 Use，不依赖 block 插入。
//   BasicBlock::addInst 只维护成员关系，不做隐式全 module 重建。
//   Module::rebuildUseDef 仅用于 reader/builder/debug repair fallback。
// ============================================================================
#ifndef SYSY_IR_BASICBLOCK_H
#define SYSY_IR_BASICBLOCK_H

#include <cstdint>
#include <string>
#include <vector>

#include "ir/Instruction.h"

namespace sysy {
namespace flat {

class Function;

// 【后置】`blockaddress` 形态的值（容器支持；dump 不打印）。
//   用途：将来需要"块地址"类语法时不必改 ValueKind（改 ValueKind 是
//   跨 3 个目录的改动 —— 铁律 7 的"不可逆决定"判据）。
class BlockAddr : public Value {
 public:
  explicit BlockAddr(const Type* ty) : Value(ValueKind::BlockAddr, ty) {}
  BasicBlock* block = nullptr;
};

class BasicBlock {
 public:
  BasicBlock(Function* parent, uint32_t index) : parent_(parent), index_(index) {}

  Function* parent() const { return parent_; }
  // 【后置】块在函数块表里的下标（**稳定**：删除块会重新编号，见
  //         `Function::removeBlock`）。dump 里的 `L<N>` 就是它。
  uint32_t index() const { return index_; }
  void setIndex(uint32_t i) { index_ = i; }
  // 【后置】可选的人类可读名字（调试/诊断用；**不参与 dump**）。
  const std::string& name() const { return name_; }
  void setName(std::string n) { name_ = std::move(n); }

  // ── 指令序列 ──────────────────────────────────────────────────────────
  const std::vector<Instruction*>& insts() const { return insts_; }
  size_t size() const { return insts_.size(); }
  bool empty() const { return insts_.empty(); }
  Instruction* at(size_t i) const { return i < insts_.size() ? insts_[i] : nullptr; }
  Instruction* back() const { return insts_.empty() ? nullptr : insts_.back(); }
  Instruction* front() const { return insts_.empty() ? nullptr : insts_.front(); }

  // 【前置】inst != nullptr 且不属于任何块。
  // 【后置】inst 追加到末尾，`inst->parent()` 指向本块。
  // 【副作用】改变指令序列；**不维护反向 use-def 边**（见文件头）。
  void addInst(Instruction* inst);
  // 【前置】inst != nullptr。
  // 【后置】inst 插入到第 i 条之前（越界则追加），`parent` 更新。
  void insertInst(size_t i, Instruction* inst);
  // 【后置】把 inst 从本块摘除（`parent` 置 nullptr）；不在本块 → 无操作。
  //         返回是否摘除成功。
  bool removeInst(Instruction* inst);
  // 【后置】清空指令表（`parent` 全部置 nullptr）。
  void clearInsts();

  // ── 参数（只对入口块有意义）────────────────────────────────────────────
  //   形参在平面层也是 `Value`，属于入口块**开头**；它们**不是指令**
  //   （`Argument : Value`），所以不进 `insts_`（否则"每条指令恰好一个终结符"
  //   的判据会被它们污染）。
  const std::vector<Argument*>& params() const { return params_; }
  size_t numParams() const { return params_.size(); }
  Argument* param(size_t i) const { return i < params_.size() ? params_[i] : nullptr; }
  void addParam(Argument* p) { params_.push_back(p); }

  // ── 终结符 ────────────────────────────────────────────────────────────
  // 【后置】最后一条指令**若**是终结符则返回它，否则 nullptr。
  Instruction* terminator() const;
  bool hasTerminator() const { return terminator() != nullptr; }

  // ── 前驱（**派生数据**，由 `Module::rebuildCFG` 重建）──────────────────
  //   为什么缓存而不是每次扫全函数：φ 的合法性检查、支配树、以及 S14 的
  //   LoopInfo 都要反复问"谁是我的前驱"。缓存 + 一个显式的重建点，
  //   比"每次 O(块数) 扫描"更不容易写错（也不会漏掉某处忘记更新）。
  const std::vector<BasicBlock*>& preds() const { return preds_; }
  size_t numPreds() const { return preds_.size(); }
  BasicBlock* pred(size_t i) const { return i < preds_.size() ? preds_[i] : nullptr; }
  void addPred(BasicBlock* b) { preds_.push_back(b); }
  void clearPreds() { preds_.clear(); }
  // 【后置】把 preds 按**块号升序**排序（φ 的入值顺序按它打印，见 dump 规则）。
  void sortPreds();

 private:
  Function* parent_;
  uint32_t index_;
  std::string name_;
  std::vector<Instruction*> insts_;
  std::vector<Argument*> params_;
  std::vector<BasicBlock*> preds_;
};

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_BASICBLOCK_H
