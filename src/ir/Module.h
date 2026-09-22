// ============================================================================
// ir/Module.h —— 模块 + **唯一所有权**
//
//   模块 = 全局变量 + 函数表（prompt §三.3）+ 全部值的**唯一所有权**。
//
// ── 为什么所有权集中在 Module ─────────────────────────────────────────────
//   * 指令、基本块、函数、全局对象一律由 `Module` `new` 出来、由 `Module`
//     析构时**逐个 delete**（迭代释放）⇒ 4000 层深 / 几万条指令的 IR 不会在
//     析构时爆栈，也不会有"谁负责释放"的扯皮（与 S05 的 Arena 同一条理由）。
//   * 于是 `Value*` / `BasicBlock*` / `Instruction*` 在 Module 生命期内
//     **永不失效** —— 平面层所有 pass 都能安全持有裸指针（D9 的"纯数据结构"
//     就是这个意思：容器不藏所有权游戏）。
//
// ── 两个显式的重建点（**不要绕过它们**）──────────────────────────────────
//   ① `rebuildUseDef()`：从零重建全部反向边（`Value::users()`）；
//   ② `rebuildCFG()`：重建每个块的前驱表（`BasicBlock::preds()`）并按块号升序。
//   构造完、读回完、以及任何"成批改动"之后都要调这两个
//   （顺序：先 CFG 后 use-def；两者互不依赖，但固定顺序能让 dump 稳定）。
//   增量接口（`addInst`/`setOperand`）**故意不自动**维护它们：让"插入一条
//   指令"变成隐式的 O(全域) 工作会让平面层的 pass 慢得莫名其妙，
//   而"什么时候重建"是**调用方的显式决定**。
//
// 【冻结】S06 结束后 `ir/` 的全部类接口**只增不改**（SESSION-PLAN §5）。
// ============================================================================
#ifndef SYSY_IR_MODULE_H
#define SYSY_IR_MODULE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/BasicBlock.h"
#include "ir/Function.h"
#include "ir/Instruction.h"
#include "ir/Type.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {

// ============================================================================
// 全局对象
// ============================================================================
class GlobalVariable {
 public:
  GlobalVariable(std::string name, const Type* objTy)
      : name_(std::move(name)), objTy_(objTy) {}

  const std::string& name() const { return name_; }
  // 【后置】对象类型（**不是**指针类型）：`i32` / `[4 x i32]` / `[2 x [3 x i32]]`。
  const Type* objType() const { return objTy_; }
  // 【后置】该全局的**地址**的类型（`ptr[objType]`）。
  const Type* addrType() const { return typePool().ptrTo(objTy_); }
  SourceLoc loc() const { return loc_; }
  void setLoc(SourceLoc l) { loc_ = l; }

  // ── 初始化器 ──────────────────────────────────────────────────────────
  //   非零元素：`字节偏移 → 32 位值`（与 S04 的初始化计划 / S05 的
  //   `GlobalVar` **同一表示**，逐字节可比）。空表 = 全零。
  //   ⚠️ 零初始化的全局**必须**显式发射 `zeroinitializer`
  //      （省略 initializer 是非法 LLVM，AGENT-CONTEXT §3.6）。
  using Data = std::vector<std::pair<uint64_t, uint32_t>>;
  const Data& initData() const { return data_; }
  bool hasData() const { return hasData_; }
  void setInitData(Data d) { data_ = std::move(d); hasData_ = !data_.empty(); }
  // 【后置】显式标记为全零（`:zero`）—— 与"数据表为空"在**语义**上等价，
  //   但在 dump 里两者都印 `zeroinitializer`（唯一的全零写法）。
  void setZero() { data_.clear(); hasData_ = false; }

 private:
  std::string name_;
  const Type* objTy_;
  SourceLoc loc_;
  Data data_;
  bool hasData_ = false;
};

// 【后置】"全局变量的地址"这个**指针值**。
//   为什么每个引用要独立的 Value 对象：dump 要给每个值分配 `%N`，
//   而同一个全局在不同函数里被引用时**各有各的编号** ——
//   共享一个 Value 会让"名字 → 值"变成多对一，读回器无法还原。
class GlobalAddr : public Value {
 public:
  GlobalAddr(GlobalVariable* gv, const Type* ptrTy)
      : Value(ValueKind::GlobalAddr, ptrTy), gv_(gv) {}
  GlobalVariable* global() const { return gv_; }

 private:
  GlobalVariable* gv_;
};

// ============================================================================
// Module
// ============================================================================
class Module {
 public:
  Module() = default;
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  ~Module() { clear(); }

  // 【后置】释放**全部**持有的对象（指令/块/函数/全局/常量/引用）。
  //         **迭代**释放，不递归。
  void clear();

  // ── 源文件基名（进 dump 头一行；**不是绝对路径** ⇒ dump 是纯函数）────
  const std::string& sourceName() const { return sourceName_; }
  void setSourceName(std::string s) { sourceName_ = std::move(s); }

  // ── 工厂（全部由 Module 持有）─────────────────────────────────────────
  // 【后置】一条新指令（未挂到任何块上）。
  Instruction* createInst(Opcode op, const Type* ty, SourceLoc loc);
  // 【后置】把已经由 `createInst`/工厂造出来的指令**登记为本模块所有**
  //         （`clear()` 会释放它）。用途：工厂 + 单独挂块的调用风格。
  //         ⚠️ 同一条指令只登记一次（重复登记会在析构时二次 delete）。
  // 【后置】登记一条指令（**幂等**：同一条只进一次）。
  //   ⚠️ 幂等不是"防御性编程"，而是**必需的**：调用方有两种自然写法
  //   （工厂造好手动登记 / 走 `emit`），两者叠加时同一条会被登记两次，
  //   而重复登记 = 析构时二次 `delete`（实测 ASAN heap-use-after-free）。
  //   代价是一次哈希插入（构建期一次性开销，不在任何遍历热路径上）。
  void ownInst(Instruction* i) {
    if (i == nullptr) return;
    if (!ownedSet_.insert(i).second) return;
    insts_.push_back(i);
  }
  // 【后置】一个新基本块（自动追加到 `fn` 的块表末尾并编号）。
  BasicBlock* createBlock(Function* fn, const std::string& name = std::string());
  // 【后置】一个新函数（追加到模块函数表末尾）。
  Function* createFunction(const std::string& name, const Type* retTy);
  // 【后置】一个新的全局对象（追加到全局表末尾）。
  GlobalVariable* createGlobal(const std::string& name, const Type* objTy);

  // 【后置】`gv` 的地址值（**同一模块内每个全局只有一个引用对象**，
  //         首用时创建）。返回 nullptr 表示 gv == nullptr。
  GlobalAddr* globalAddr(GlobalVariable* gv);

  // ── 函数 / 全局表 ─────────────────────────────────────────────────────
  const std::vector<Function*>& functions() const { return funcs_; }
  Function* function(size_t i) const { return i < funcs_.size() ? funcs_[i] : nullptr; }
  Function* findFunction(const std::string& name) const;
  size_t numFunctions() const { return funcs_.size(); }

  const std::vector<GlobalVariable*>& globals() const { return globals_; }
  GlobalVariable* global(size_t i) const { return i < globals_.size() ? globals_[i] : nullptr; }
  GlobalVariable* findGlobal(const std::string& name) const;
  size_t numGlobals() const { return globals_.size(); }

  // ── 常量（**按位模式去重**）───────────────────────────────────────────
  // 【前置】ty == i32()/i64()。浮点用下面的 `getFloatConst`。
  Constant* getIntConst(const Type* ty, int64_t v);
  // 【前置】bits 是 f32 的 IEEE-754 原始位。
  Constant* getFloatConst(uint32_t bits);

  // ── 两个重建点 ────────────────────────────────────────────────────────
  // 【后置】每个块的 `preds()` = 全函数扫描得到的真实前驱，按块号升序。
  void rebuildCFG();
  // 【后置】每个 `Value` 的 `users()` 与全部指令的 `operands()` 一致
  //         （**双向**）。构造完 / 读回完 / 成批改动后必须调用。
  void rebuildUseDef();

  // ── 统计（轨 C：块/指令/φ 数分布；`--dump-flat-stats` 用它）──────────
  struct Stats {
    size_t funcs = 0;
    size_t blocks = 0;
    size_t insts = 0;      // 不含形参占位
    size_t phis = 0;
    size_t params = 0;
    size_t globals = 0;
  };
  Stats stats() const;
  // 【后置】人类可读 + 机器可 grep 的统计文本（**不写进 dump**：dump 是冻结
  //   契约，多一行就破坏逐字节比对；与 S05b 的 `--dump-loopnorm-stats` 同理）。
  std::string formatStats() const;

  // 【后置】自增的值编号（dump 用；保证"同一个值拿到同一个编号"）。
  uint64_t nextValueId() { return nextId_++; }

 private:
  std::string sourceName_;
  std::vector<Instruction*> insts_;
  std::vector<BasicBlock*> blocks_;
  std::vector<Function*> funcs_;
  std::vector<GlobalVariable*> globals_;
  std::vector<Constant*> consts_;
  std::vector<GlobalAddr*> globalAddrs_;
  std::unordered_set<Instruction*> ownedSet_;               // ownInst 的幂等依据
  std::unordered_map<std::string, Constant*> intConsts_;    // "i32:5" → 常量
  std::unordered_map<uint32_t, Constant*> floatConsts_;     // 位模式 → 常量
  std::unordered_map<GlobalVariable*, GlobalAddr*> addrOf_;  // 全局 → 地址值
  uint64_t nextId_ = 1;
};

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_MODULE_H
