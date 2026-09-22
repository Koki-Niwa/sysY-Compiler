// ============================================================================
// structured/FlattenInternal.h —— FlattenCFG 的**实现私有**细节（不对外）
//
//   ⚠️ 命名约定与 `IRGenInternal.h` 相同：`FlattenCFG.h` 只暴露
//      `flattenModule`（唯一入口），`FlatBuilder` 是本目录内部的东西。
//   为什么要这个头：`FlattenCFG.cpp`（块骨架）与 `FlattenLower.cpp`
//   （单个 Op 的降级）**都是 `FlatBuilder` 的成员函数定义**，必须共享同一个
//   类声明。拆两个文件是为了 §C4 的单文件行数上限。
//
// ── 遍历模型（**显式帧栈**，不用递归）────────────────────────────────────
//   帧有两种（`FrameKind`）：
//     * `Region`：把某个 Region 里剩下的 Op 依次降级，走完跳到 `cont`；
//       `Yield` 被特殊处理（它要跳到"这个容器该去的地方"，可能是循环头）；
//     * `Finish`：一个**收尾动作**（`finish` 闭包）。用途是"两件事都做完
//       之后再干第三步" —— 例如 `If` 要先展开两个分支，才能建汇合块的 φ；
//       `For` 要等体展开完，才能填"循环携带变量"φ 的回边入值。
//   ⚠️ 收尾帧是**必需**的，不是设计冗余：循环携带的 φ 在"建循环头"时
//      就知道**进入边**的入值，但**回边**的入值要等体展开完 —— 没有收尾帧
//      就只能回头改已经发出的指令（那会让"指令一旦发出就不再变"这条
//      简化假设失效）。
//
// ── 本类的职责边界（写清楚，免得两个文件互相越界）────────────────────────
//   * `FlattenCFG.cpp`：函数的块骨架 —— 入口块、If/While/For 的展开、
//     汇合点的 φ 放置、循环出口的合并、临界边拆分、全局与函数列表的搭建。
//   * `FlattenLower.cpp`：**单个 Op** 的降级 —— 常量、内存、算术、比较、
//     转换、调用、终结符。它只知道"把这一条 Op 变成平面指令"。
// ============================================================================
#ifndef SYSY_STRUCTURED_FLATTENINTERNAL_H
#define SYSY_STRUCTURED_FLATTENINTERNAL_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/Module.h"
#include "structured/FlattenCFG.h"

namespace sysy {
namespace flat {

using sir::Op;
using sir::OpKind;
using sir::Region;

// 【后置】结构化层的类型 → 平面层的类型（同一个形状、同一个类型池）。
const Type* toFlatType(const sir::Type* t);

// 降级动作的结果。`kStop` = 致命错误（已报诊断，外层放弃产物）。
enum class Action { kOk, kStop };

// "这一帧不需要环境快照"（`Finish` 帧与顶层帧用它）。
constexpr uint32_t kNoEnv = 0xffffffffu;

// 预算（安全阀）：超出**先报 error 再放弃**，绝不静默（prompt §八）。
constexpr size_t kMaxBlocksPerFunc = 200000;
constexpr size_t kMaxInstsPerFunc = 4000000;

class FlatBuilder {
 public:
  FlatBuilder(Module& m, DiagnosticEngine& d) : m_(m), diag_(d) {}

  // 【前置】`fn->kind == OpKind::Func`。
  // 【后置】把 `fn` 展平成一个平面函数并返回（形参在入口块开头）。
  Function* run(const Op* fn);

  // 【前置】`modRegion` 是 ModuleOp 的 Region。
  // 【后置】把**模块级**的 Op 降级并绑好值（目前只有 `GetGlobal`）。
  //   ★ 为什么必须有这一步：`GetGlobal` 在结构化层是**模块 Region 里的 Op**
  //     （不在任何函数体里），而函数体里到处引用它的结果（`%.0`）——
  //     第一版只展平函数体 ⇒ 所有 `GetGlobal` 的结果都没绑上，
  //     引用它的 `gep`/`load` 全部报"操作数缺失"（实测 `09-arrays`）。
  //   ⚠️ 必须与函数展开**共用同一个** `FlatBuilder`（同一个 `vals_`）：
  //     分成两个实例时，"在这个实例里绑好、在另一个实例里查"必然查不到
  //     （实测又踩了一次）。
  void lowerModuleRegion(const Region* modRegion);

  bool failed() const { return failed_; }

 private:
  enum class FrameKind : uint8_t { Region, Finish };

  struct FinishState {
    BasicBlock* head = nullptr;    // while：循环头（条件块）
    BasicBlock* body = nullptr;    // while：体入口（回边目标）
    BasicBlock* inc = nullptr;     // for：自增块
    BasicBlock* preB = nullptr;    // for：循环前块（φ 的进入边）
    BasicBlock* exit = nullptr;    // 循环出口
    BasicBlock* ivSlot = nullptr;  // for 的 IV 槽
    Value* ivPhi = nullptr;        // for 的 IV φ
    Value* step = nullptr;         // for 的步长
    std::vector<Value*> carrySlots;
    std::vector<Instruction*> carryPhis;
    SourceLoc loc;
    int kind = 0;                  // 0 = if 汇合, 1 = while 收尾, 2 = for 收尾
  };

  struct Frame {
    FrameKind kind = FrameKind::Region;
    Region* region = nullptr;
    size_t i = 0;
    BasicBlock* cont = nullptr;    // Region 走完的续点
    std::function<void()> finish;  // Finish 帧的动作
    // 循环上下文（Region 帧用）：
    BasicBlock* loopExit = nullptr;   // `Break` 跳这里
    BasicBlock* loopHead = nullptr;   // `Yield` 跳这里（while 条件→体；for 体→inc）
    bool inLoop = false;
    bool isWhileCond = false;         // 这是 while 的**条件** Region
    std::vector<Instruction*> pendingCarry;   // while：待补回边入值的 φ
    std::vector<Value*> pendingCarrySlots;
    // ★ 这个 Region 走完时的环境快照放在 `frameEnvs_[envSeq]`。
    //   为什么不是"块 → 环境"的表：嵌套的控制流会让外层汇合块与内层的分支
    //   块**共享**同一张表 —— 实测症状就是"内层 if 一跑，外层 then/else 的
    //   环境快照全被覆盖成空/错的值"。用**帧自己的序号**做键之后，
    //   "谁的环境"由栈位置唯一决定，与块的编号无关。
    uint32_t envSeq = kNoEnv;
    // ★ 这个 Region 走完时的**出口块**与**出口环境**，放在
    //   `frameExitBlocks_[finishSlot]` / `frameExitEnvs_[finishSlot]`。
    //   为什么不是"进来时那个块"：Region 里若有一个**控制流容器**（`if`），
    //   它会在最后留下自己的汇合块 ⇒ 真正"走到这里"的块变成了那个汇合块。
    //   实测症状：外层 `if` 的汇合块 φ 把入边写成内层 `then` 块（L1），
    //   而真实前驱是内层 `if` 的汇合块（L6）⇒ V3 报"前驱集合不同"。
    int finishSlot = -1;
    // 额外要记"出口块"的槽位（循环体的**回边源块**要等体走完才知道）
    int backedgeSlot = -1;
    // 这个 Region 里**最后一条指令发在哪个块**（= fall-through 的落点）。
    //   与"出口块"的区别：出口块是"控制流最后走到哪儿"（`return` 之后可能是
    //   一个返回块），而回边/汇合的**源块**要的是"指令流在哪里结束"。
    //   实测（`fft0.sy` 的 `multiply`）：体的最后一条是 `Return`（在一个
    //   嵌套 `If` 的分支里），出口块是那个返回块，而回边源块是体的末尾块。
    BasicBlock* streamEnd = nullptr;
    // ★ 这个 Region **开始时**所在的块（`cur_`）。
    //   为什么必须有：Region 走完后要"补一条跳转到续点"，但**不能无条件补** ——
    //   若这个 Region 的终结符已经把这个块终结了（`break`/`return`/
    //     `unreachable`/`if` 的自分支跳转），再补一条就会让块里出现
    //   **两条终结符**（实测被 V4 抓出来：`while + if-break` 的 then 块）。
    //   `cur_` 在嵌套构造之后已经指向别的块了，所以必须记"起始块"。
    BasicBlock* startBlock = nullptr;
  };

  using Env = std::unordered_map<Value*, Value*>;   // 槽 → 当前值

  // ── 骨架（FlattenCFG.cpp）───────────────────────────────────────────────
  BasicBlock* newBlock(const std::string& name);
  void enterBlock(BasicBlock* b, const Env& env);
  // ⚠️ `inEnvs` **按值**收（不是 `const Env*`）：调用方给的多半是
  //   `blockEnv_` 里的元素引用，而本函数末尾要写 `blockEnv_` ——
  //   `unordered_map` 的插入可能让那些引用失效（实测踩到过）。
  void enterJoin(BasicBlock* b, std::vector<Env> inEnvs, SourceLoc loc,
                 const std::vector<BasicBlock*>& edgeBlocks);
  void walk(Region* body);
  Action lowerIf(Op* op, Frame& fr);
  Action lowerWhile(Op* op, Frame& fr);
  Action lowerFor(Op* op, Frame& fr);
  void splitCriticalEdges();
  void collectReadSlots(Region* region);
  void collectStoredSlots(Region* region, std::unordered_set<Value*>& out) const;

  // ── 单条 Op 的降级（FlattenLower.cpp）─────────────────────────────────
  Action lower(Op* op);
  bool lowerTerminator(Op* op, Frame& fr);
  void fillLoopBackedges(FinishState& st);

  // ── 小工具 ─────────────────────────────────────────────────────────────
  // 【后置】把指令登记进模块（**幂等**：同一条只登记一次）。
  //   为什么必须幂等：常量走池化复用时会被反复"顺路登记"，
  //   而重复登记 = 析构时二次 `delete`（实测 `free(): double free`）。
  void ownInst(Instruction* i) {
    if (i == nullptr) return;
    if (!owned_.insert(i).second) return;
    m_.ownInst(i);
  }
  Instruction* emit(Instruction* i);
  bool hasTerm(BasicBlock* b) const { return b != nullptr && b->hasTerminator(); }
  Value* map(sir::Value v) const;
  void bind(sir::Value v, Value* fv);
  Value* kInt(int32_t v, SourceLoc loc);
  Value* kI64(int64_t v, SourceLoc loc);
  Value* kFlt(uint32_t bits, SourceLoc loc);
  Value* constInt(int64_t v, const Type* ty, SourceLoc loc);
  Value* zeroOfSlot(Value* slot, SourceLoc loc);
  // 【后置】`v` 若**与函数返回类型不符**且是"字面量 0"，返回一个**正确类型**的 0；
  //   否则原样返回 `v`。
  //   ★ 为什么需要：IRGen 会给"函数体末尾没有 return"的函数补一条兜底
  //     `return 0`（S05 的行为），而那个 `Int 0` 在结构化层的类型是 `i32`
  //     —— `float f(){ if(c) return a; else return b; }` 这种（真的缺末尾
  //     return）会产出 `ret i32 0` 而签名是 `f32` ⇒ V4 报"ret 的值类型与
  //     函数返回类型不符"。实测：`39_fp_params.sy` 的 `params_f40`。
  //   ⚠️ 只对"0"做这件事：非 0 的不符是**真**的类型错误，不许静默改。
  Value* retValueFor(Value* v, SourceLoc loc);
  // 【后置】取"循环前求值一次"的操作数；若它就是 IV 的 φ，则在**当前块**
  //         （= 循环前块）重发一条 load（见 FlattenLower.cpp 的说明）。
  // 【前置】`bb` 是循环前块（`cur_` 可能已经不是它了 —— `newBlock` 会改 `cur_`）。
  Value* preheaderValue(sir::Value v, Value* phi, const Type* ty, BasicBlock* bb,
                        SourceLoc loc);
  // 【后置】`slot`（平面值）对应的结构化槽是否**被读过**（判据 2）。
  //   实现：平面槽 → 结构化值的反查 + 预扫得到的结构化"被读"集合。
  //   为什么不在建平面槽的那一刻判定：判据 2 是"整个函数范围内被读过"，
  //   而不是"到这里为止被读过"（先写后读的变量也必须进环境）。
  bool isReadSlot(Value* slot) const;
  // 【后置】`v` 是不是"本地槽"（某个 `alloca` 的结果）。
  //   只有本地槽才是"变量在内存里的版本"（判据 2 的对象）；
  //   全局对象的地址（`GlobalAddr`）与常量池地址不参与 φ。
  bool amLocalSlot(Value* v) const;
  // 【后置】`v` 的**根槽**：`v` 本身是 `alloca` 结果就返回 `v`；
  //   `v` 是"从某个 alloca 结果算出来的 GEP / bitcast"就返回那个 alloca 结果；
  //   其余（全局地址、参数、调用结果…）返回 nullptr。
  //   为什么需要它：平面层的变量地址是 `gep alloca, 0`（不是 alloca 本身），
  //   而"槽的类型"与"槽的当前值"都必须记在**根槽**上，否则同一块内存会有
  //   两个身份（实测：`t = i * 2` 的 store 把类型记在 GEP 结果上、
  //   而 `load t` 查的是 alloca 结果 ⇒ 查不到、类型回退成错的那个）。
  Value* rootSlot(Value* v) const;
  const Type* slotType(Value* slot) const;
  void setSlot(Value* slot, Value* v);
  void giveUp(const std::string& msg, SourceLoc loc);

  Module& m_;
  DiagnosticEngine& diag_;
  Function* f_ = nullptr;
  BasicBlock* cur_ = nullptr;
  std::vector<BasicBlock*> blocks_;
  Env env_;
  // ★ "被读过的槽"用**结构化层的值**做键（不是平面值）。
  //   为什么：这一步必须在**展平之前**扫完（"先读后写"的槽也要进环境），
  //   而那时平面值还不存在（`map()` 全是 nullptr）——第一版就是在这里错的：
  //   集合里塞满了 nullptr ⇒ `setSlot` 永远不进环境 ⇒ **一个 φ 都不放**。
  std::unordered_set<sir::Value> readSlotsSir_;
  // 平面槽 → 它来自哪个结构化 Alloca 结果（判据 2 的查询用）
  std::unordered_map<Value*, sir::Value> sirOfSlot_;
  // ★ 平面槽 → 它**装的是什么类型**（来自"写它的那条 store 的元素类型"）。
  //   为什么不能信 `LoadOp` 的 `<ty>` 属性：结构化层对**指针变量**
  //   （数组形参 `int a[]`、数组对象 `int loc[3]`）的属性是
  //   `Load(ptr[i32])`，也就是"指针的元素类型的元素类型"——
  //   实测 `(Alloca %pick.1 ptr[i32]) (Load %pick.7 i32 %pick.6)`：
  //   在 `%pick.6 : ptr[i32]` 上 load `i32` 是**自相矛盾**的，而
  //   `store` 的属性（`Store ptr[i32] …`）是对的。平面层是 typed pointer，
  //   不能把这个矛盾带过来（后端要靠 `load` 的类型发射）。
  //   ⇒ 以 **store 的元素类型**为准（每个槽第一次被写时记下）。
  std::unordered_map<Value*, const Type*> slotTy_;
  // ★ "这个槽里装的是指针/数组"（数组形参 `int a[]` / 局部数组对象）。
  //   为什么单独标出来：\(arphi\) 判据 2 的"环境"提升对**指针值的槽**会
  //   改变 IR 的形状（把 `load` 换成参数值本身），而结构化层对指针槽的
  //   `load` 类型属性是错的 ⇒ 两边都不划算。本关**不提升指针槽**：
  //   它们照旧 `load`/`store`（S09 的 mem2reg 会处理它），语义与类型都不会错。
  std::unordered_set<Value*> ptrSlots_;
  // ★ 常量池：**(类型, 位模式) → 常量指令**（dump 规则 §4.3 要求去重）。
  //   不去重的后果（独立实现轨 D 抓出来的**最大一类差异**）：同一条 `i32 0`
  //   被反复定义十几次，既让 dump 膨胀、又让 `%N` 编号整体错位。
  //   池的**键里带函数名**：常量指令是"属于某个函数"的（dump 把定义行印在
  //   函数里），跨函数共用一个常量会让它出现在错误的函数中。
  std::unordered_map<std::string, Instruction*> constPool_;
  // 已登记的全部指令（`ownInst` 幂等的依据）
  std::unordered_set<Instruction*> owned_;
  // 常量池里的键值类型是 `Instruction*`（常量是**指令形态**）
  // ★ 全局地址池：**全局名 → 地址值**（每个全局只物化一个值，
  //   否则同一根全局指针在不同位置拿到不同编号 ⇒ dump 不稳定）。
  std::unordered_map<std::string, Value*> globalPool_;
  std::unordered_map<sir::Value, Value*> vals_;
  std::unordered_map<sir::Value, Instruction*> params_;
  // 当前循环里出现的 `break`：出口块 + 那一刻的环境（供出口块的 φ 合并）
  std::vector<std::pair<BasicBlock*, Env>> breaks_;
  // ★ Region 走完时的环境快照，按**帧序**编号（`frameEnvSeq_` 是分配器）。
  //   为什么用序号而不是指针：`std::vector<Frame>` 在压栈时会搬动元素
  //   ⇒ "指向帧的指针"不可靠；序号没有这个问题，而 finish 闭包按值捕获序号。
  std::vector<Env> frameEnvs_;
  uint32_t frameEnvSeq_ = 0;
  // 每个 Region 的"出口块 / 出口环境"（按 `finishSlot` 索引）
  // 哨兵：这个 Region 的出口**已知走不到续点**（两个分支都 return/break）
  static BasicBlock* const kDeadExit;
  std::vector<BasicBlock*> frameExitBlocks_;
  std::vector<Env> frameExitEnvs_;
  // 【后置】分配一个"出口块/出口环境"槽位。
  int allocFinishSlot() {
    const int s = static_cast<int>(frameExitBlocks_.size());
    frameExitBlocks_.push_back(nullptr);
    frameExitEnvs_.emplace_back();
    return s;
  }
  // 【后置】`slot` 这个 Region 的出口块**是否真的会走到 `target`**。
  //   判据：出口块还没终结（收尾帧会给它补一条跳转）**或者**它已经以
  //   `br target` 终结。
  //   ★★ 为什么必须有这个判据 ★★
  //     `lowerIf` 原来无条件把分支的**出口块**当成汇合块的一条入边 ——
  //     但分支里若有循环，它的出口块是**循环的出口**，而循环出口会
  //     `br` 回**循环头**（而不是汇合块）⇒ 那条边**不存在**，
  //     φ 却宣称它存在 ⇒ "φ 的入值个数与前驱个数不符"（实测：`multiply`
  //     的 `L6 want={L5} got={L5 L9}`）。这与平面执行器里"φ 按真实前驱选"
  //     是**同一个误解**（今天第三次遇到）。
  //   ⚠️ 判据的**唯一**形式就是"出口块自己会不会跳到 target"：
  //     · `exitBlockOf` 返回 nullptr = 这个 Region 的出口块已经终结且**不**
  //       落到续点（`return`/`break`/回边）⇒ **不算**入边。★ 不能退回去用
  //       "分支的起始块"当兜底 —— 起始块是新建的空块，它**恰好**满足
  //       "还没终结" ⇒ 会把一条不存在的边算进去（实测：`52_scope.sy` 的
  //       `func` 两个分支都 `return`，汇合块 L3 **不可达**、真实前驱为空，
  //       φ 却拿着 `{L1 L2}`）。
  bool reachesTarget(int slot, BasicBlock* target) const {
    BasicBlock* b = exitBlockOf(slot);
    if (b == nullptr) return false;
    Instruction* t = b->terminator();
    if (t == nullptr) return true;                  // 还没终结 ⇒ 会补跳转
    if (t->op() != Opcode::Br || t->numSuccs() != 1) return false;
    return t->succ(0) == target;
  }

  BasicBlock* exitBlockOf(int s) const {
    return (s >= 0 && static_cast<size_t>(s) < frameExitBlocks_.size())
               ? frameExitBlocks_[static_cast<size_t>(s)]
               : nullptr;
  }
  // 【后置】把槽 `s` 标成"**走不到续点**"（哨兵值 `kDeadExit`）。
  //   为什么需要哨兵，而不是直接写 `nullptr`：`nullptr` 是"槽**还没被填**"
  //   的初值，收尾帧 `walk` 最后还会无条件把 `cur_` 写进去 —— 于是"死掉"的
  //   事实会被覆盖，收尾帧再补一条跳转 ⇒ **块里两条终结符**（实测 `52_scope`
  //   的 `func`：L3 同时有 `unreachable` 和 `br`）。哨兵让"已知死掉"与
  //   "还不知道"分得开。
  void markExitDead(int s) {
    if (s >= 0 && static_cast<size_t>(s) < frameExitBlocks_.size()) {
      frameExitBlocks_[static_cast<size_t>(s)] = kDeadExit;
    }
  }
  bool exitIsDead(int s) const { return exitBlockOf(s) == kDeadExit; }
  const Env& exitEnvOf(int s) const {
    static const Env kEmpty;
    return (s >= 0 && static_cast<size_t>(s) < frameExitEnvs_.size())
               ? frameExitEnvs_[static_cast<size_t>(s)]
               : kEmpty;
  }
  // 【后置】为"要走完一个 Region"的帧分配一个快照槽位。
  //   调用方必须在**压栈之前**调用它（`frameEnvs_` 会扩容，但不能在
  //   `finish` 闭包已经捕获序号之后失效 —— 序号是下标，扩容不影响）。
  uint32_t allocEnvSeq() {
    const uint32_t s = frameEnvSeq_++;
    if (frameEnvs_.size() <= s) frameEnvs_.resize(s + 1);
    return s;
  }
  // 【后置】取某个快照（越界/未分配 → 空环境）。
  const Env& envOfSeq(uint32_t s) const {
    static const Env kEmpty;
    return s < frameEnvs_.size() ? frameEnvs_[s] : kEmpty;
  }
  // 帧栈（**成员**，不是 `walk()` 的局部量）。
  //   为什么：`lowerIf`/`lowerWhile`/`lowerFor` 要往栈里压帧，而它们的调用点
  //   正拿着栈里的一个 `Frame&` —— `push_back` 可能让那个引用失效
  //   （这是真实的悬垂来源，实测被 `-Wdangling-pointer` 抓出来过：
  //    曾经用 `std::vector<Frame>* stack_` 指向局部栈，编译器直接报
  //    "storing the address of local variable"）。
  //   做成成员之后，`walk()` 只按下标访问 `stack_.back()`，压栈/弹栈都不留引用。
  std::vector<Frame> stack_;
  size_t instCount_ = 0;
  // 【后置】"已经展平过至少一个函数"（用于区分"第一个函数"与"后续函数"：
  //   模块级状态只在第一轮之前保留，函数级状态每轮重置 —— 见 `run()` 的说明）。
  bool started_ = false;
  bool failed_ = false;
  bool overflowed_ = false;
};

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_STRUCTURED_FLATTENINTERNAL_H
