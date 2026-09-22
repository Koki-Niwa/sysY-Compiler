// ============================================================================
// ir/Function.h —— 函数
//
//   函数 = 参数表 + 基本块表（**入口块第一条**）+ 每个值的定义点（prompt §三.3）。
//
// ── 三个不变量（后端契约，见 AGENT-CONTEXT §5）───────────────────────────
//   ① **入口块恒为 `blocks()[0]`**（后端据此放函数序言）；
//   ② 每个块**恰好一个终结符**，且是**最后一条**指令；
//   ③ `alloca` **全部在入口块**（不变量 2；S05b 的 AllocaHoist 已保证，
//      FlattenCFG 也不许把它们挪走）。
//
// ── 值的定义点 ────────────────────────────────────────────────────────────
//   `defs_[name]` 是"名字 → 定义它的值"的**模块内唯一命名空间**
//   （参数名、函数名、全局名）。平面 IR 的**值名是文本层的概念**
//   （`%0`/`%1`/… 由 dump 按先序分配，见 dump 规则 §4.1），所以这里存的是
//   **实体名**（`main`、`g`、`i32` 参数的无名占位不在此列）。
// ============================================================================
#ifndef SYSY_IR_FUNCTION_H
#define SYSY_IR_FUNCTION_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir/BasicBlock.h"
#include "ir/Type.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {

class Module;

class Function {
 public:
  Function(Module* parent, std::string name, const Type* retTy)
      : parent_(parent), name_(std::move(name)), retTy_(retTy) {}

  Module* parent() const { return parent_; }
  const std::string& name() const { return name_; }
  const Type* retType() const { return retTy_; }
  void setRetType(const Type* t) { retTy_ = t; }
  // 【后置】该函数是不是**声明**（没有基本块）。
  bool isDeclaration() const { return blocks_.empty(); }
  SourceLoc loc() const { return loc_; }
  void setLoc(SourceLoc l) { loc_ = l; }

  // ── 参数 ──────────────────────────────────────────────────────────────
  const std::vector<Instruction*>& params() const { return params_; }
  size_t numParams() const { return params_.size(); }
  Instruction* param(size_t i) const { return i < params_.size() ? params_[i] : nullptr; }
  // 【前置】p != nullptr。入口块必须已存在（参数挂在入口块开头）。
  // 【后置】p 追加为最后一个形参，同时挂在入口块的 params 表里。
  void addParam(Instruction* p);
  const Type* paramType(size_t i) const {
    Instruction* p = param(i);
    return p != nullptr ? p->type() : nullptr;
  }

  // ── 基本块 ────────────────────────────────────────────────────────────
  const std::vector<BasicBlock*>& blocks() const { return blocks_; }
  size_t size() const { return blocks_.size(); }
  BasicBlock* at(size_t i) const { return i < blocks_.size() ? blocks_[i] : nullptr; }
  // 【后置】入口块（**恒为 blocks()[0]**；空函数返回 nullptr）。
  BasicBlock* entry() const { return blocks_.empty() ? nullptr : blocks_.front(); }
  // 【前置】b != nullptr。
  // 【后置】b 追加到块表末尾，并得到下标 `index() == 原 size`。
  void addBlock(BasicBlock* b);
  // 【前置】b 属于本函数。
  // 【后置】b 从块表摘除；**其后所有块的 index 前移 1**（dump 的 `L<N>`
  //         依赖它，所以删除后必须调 `Module::rebuildCFG()`）。
  bool removeBlock(BasicBlock* b);
  // 【后置】按当前块表顺序重排全部块的 index（0..n-1）。
  void reindexBlocks();

  // ── 值的定义点（实体名 → 值）──────────────────────────────────────────
  void bindName(const std::string& name, Value* v) { defs_[name] = v; }
  Value* lookupName(const std::string& name) const {
    const auto it = defs_.find(name);
    return it == defs_.end() ? nullptr : it->second;
  }
  const std::unordered_map<std::string, Value*>& nameDefs() const { return defs_; }

  // 【后置】函数内全部指令（含终结符）的条数（不含形参占位）。
  size_t instCount() const;
  // 【后置】φ 的条数。
  size_t phiCount() const;
  // 【后置】基本块数。
  size_t blockCount() const { return blocks_.size(); }

 private:
  Module* parent_;
  std::string name_;
  const Type* retTy_;
  SourceLoc loc_;
  std::vector<Instruction*> params_;
  std::vector<BasicBlock*> blocks_;
  std::unordered_map<std::string, Value*> defs_;
};

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_FUNCTION_H
