// ============================================================================
// InitLowering.cpp —— 初始化器降级（S04）
//
// 接口与前置条件见 InitLowering.h。本文件的四段：
//   一、展平：C11 §6.7.9 的"当前对象"模型（**显式工作栈**）
//   二、常量判定：调用 ConstEvaluator（**不许写第二份求值**）
//   三、计划构造：全局"默认值 + 稀疏非零"、局部"零填充 + 常量段 + 表达式"
//   四、遍历：顶层 / 函数体 / 语句（**显式工作栈**，不递归）
//
// ── ★ 与 S03（SemaDecl.cpp 的 doInitGroup）的关系：同一套模型，两处实现 ──
//   这是本模块**唯一**的"重复知识"，必须交代清楚：
//     * S03 的 `doInitGroup` 用同一套游标模型，但它的目的是**判定合法性**
//       （报 E-INIT-SHAPE），而且它有一处**已知偏差**：嵌套组占用的子对象
//       大小取自 `elemSizeAt(target, level)`（= 从该维起的全部元素数），
//       而不是"它实际绑定的那一层"的大小。于是
//           `int a[3][2] = {1,{2,3},4,5,6};`
//       被 S03 报成"元素个数超过数组元素总数"而不是"作用在标量上的组里有
//       2 个元素"——**照样是 E-INIT-SHAPE（合法性判定正确），只是原因说得不准**。
//       本模块按精确模型实现（组的容量 = 它真正绑定的那一层的元素数）。
//       两者的差异**只在非法程序**上出现：对 S03 接受的所有初始化器，本模块
//       产出与 C 逐元素一致的结果（单元测试与报告 §3 有实测）。
//     * prompt §一 第 4 条禁的是"**第二份常量求值**"（字面量进制解析、
//       `+ - * / %` 折叠、UB 归一化）—— 本模块**一次都没有**复制它，全部调用
//       `ConstEvaluator`。而"初始化语义"本身就是这一关的交付内容。
// ============================================================================
#include "frontend/InitLowering.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "frontend/ConstEval.h"
#include "support/Diagnostic.h"

namespace sysy {
namespace {

// ── 字节宽度：SysY 的标量只有 int / float，都是 4 字节（规范 §1 Overview）──
constexpr uint64_t kScalarBytes = 4;
// "连续常量段 ≥ 4 个元素 ⇒ 一条 MemcpyConst"（prompt §4.2 的策略表）。
constexpr size_t kMemcpyMin = 4;

// 第 level 层的**子对象**占几个元素 = elemSizeAt(level+1)（level >= rank ⇒ 1）。
// ★ S04 实测踩过：写成 elemSizeAt(level) 会让每个组只前进 1 格后又跳 2 格，
//   期望值表整体错位（被 check_initplan.py 的逐元素比对抓出来）。

int64_t elemSpanAt(const Type* arr, int level) {
  if (level < 0) return 1;
  const Type* t = dropDims(arr, level);
  if (t == nullptr || !t->isArray()) return 1;   // 标量位置
  return elementCount(t->elem);
}

// 第 idx 个元素（行主序扁平下标）相对对象起始的**字节**偏移。
uint64_t byteOffsetOf(int64_t idx) { return static_cast<uint64_t>(idx) * kScalarBytes; }

// ── 展平的结果：**只记被显式写到的位置**（行主序下标 → 表达式）─────────────
//   ★ 不用"每元素一格"：`const int a[50000000] = {1};` 是合法程序，稠密格子要
//     8B × 5×10^7 = **400 MB**（实测峰值 383.6 MB，预算 32 MB）；没写到的位置
//     语义上就是 0（规范 §3 ConstDef 6.3），不必存。稀疏表按下标升序（下钻时
//     可能乱序，最后排一次序），与 `InitPlan::GlobalData::nonzero` 同一种思路。
struct FlatSlot {
  int64_t index = 0;
  const Expr* expr = nullptr;
};
using FlatSlots = std::vector<FlatSlot>;

// ============================================================================
// 一、展平（C11 §6.7.9 的"当前对象"模型；**迭代**，`{{{{…}}}}` 不崩）
//
//   记号（`shape = [d0, d1, …]`、`r = rank(shape)`）：
//     * `dim[k]`  = 第 k 维的长度（k < r）
//     * `span[k]` = 第 k 层**一个对象**占几个元素
//                   = ∏_{j>=k} dim[j]（`span[r]` = 1 = 一个标量，
//                     `span[0]` = 整个数组；即"最浅一层"到"最深一层"）
//
//   模型：对象 = `(level, base)` —— 第 level 层的一个子对象，起点是扁平下标
//   `base`。`level == r` 表示**标量**对象（容量 1）；`level < r` 时它的第 e 个
//   子对象是 `(level+1, base + e·span[level+1])`。只有两个动作：
//
//     ① 花括号组作用在某个对象上，把组里的孩子**依次**摊给它的子对象：
//          for (i = 0, e = 0; i < g->list.size() && e < dim[level]; e++)
//            孩子是表达式 ⇒ 从当前子对象**借用**（见 ②），i = 借用后的位置；
//            孩子是花括号 ⇒ 作用在当前子对象上，i++；
//        `level == r`（标量对象）容量 1 ⇒ 只认**第一个**孩子（多余的 Sema 已报
//        E-INIT-SHAPE）：是表达式就写它，是花括号就剥掉首孩子继续 —— 这一步用
//        while 剥链，所以 `{{{{…1…}}}}` 十万层也只占 O(1) 内存、不占调用栈。
//
//     ② 借用（C11 §6.7.9p20 的 "Otherwise" 子句：only enough initializers
//        from the list are taken to account for the elements of the
//        subaggregate）：当前子对象是**聚合**、而下一个孩子是**裸表达式**时，
//        不新开花括号作用域，而是拿外层流的剩余孩子按行主序把这个子对象填满；
//        填满（e == dim[level]）或流用尽就回到外层，外层从用掉的位置继续。
//
//   ★ 表里最难的三类都由这两条解释（只按"游标对齐"一条规则必然错一类）：
//       `int[3][2] = {1,2,{3},5,6}`      ⇒ 1 2 3 0 5 6：`1,2` 靠借用独占 a[0]，
//                                           `{3}` 才落在一整行 a[1] 上；
//       `int[2][2] = {1,{2},3}`          ⇒ 1 2 3 0：`{2}` 落在**标量** a[0][1]
//                                           上（游标 1 对 span[1] = 2 不对齐）——
//                                           "最深对齐层"在这里对、在
//                                           `{{1},{2}}`(⇒1 0 2 0) 那里错；
//       `int[2][3][4] = {1,2,3,4,{5},{}}` ⇒ 1 2 3 4 5 0…：`1..4` 一路借到
//                                           a[0][0]，`{5}` 才落在 a[0][1] 上。
//   ★ 另两条实测踩过的坑：
//       * 组的跨度必须是"它真正绑定的那一层"的大小 `span[level]`，写成
//         "从该维起的元素数"就会让 `{{1,2},{3,4}}` 整张期望值表错位；
//       * 组内元素多于该子对象容量时**丢弃**多余（不是溢写到下一个子对象）：
//         clang -std=c99 实测 `int[2][2] = {{1,2,3,4}}` 是 1 2 0 0。
//
//   迭代化：栈 = 正在填充的对象链，每帧 = (孩子流, level, base, e, i, 借用?)。
//     子帧的 level 恒等于父帧 level+1（到 r 为止）⇒ **栈深 ≤ r+1，内存 O(rank)**，
//     与花括号嵌套深度**无关**。帧结束时回填父帧：借用子帧 ⇒ 父帧 i = 子帧 i；
//     花括号子帧 ⇒ 父帧 i++、e++。
// ============================================================================
namespace {

class Flattener {
 public:
  Flattener(const Type* target, std::vector<FlatSlot>& out)
      : out_(out), r_(rank(target)), total_(elementCount(target)) {
    // ⚠️ 容量必须**随秩**走，不能固定成 8：语料里有 **19 维**数组
    //    （`h_functional/30_many_dimensions.sy`）。早先 `span_[8]` + `r>7 就 return`
    //    让"秩 ≥8 且初始化器非零"静默产出全零计划——又一次"保护性截断不报错"。
    //    语料那两个恰好是 `= {0}`，所以三轮验收全绿也照不出来。
    dim_.assign(static_cast<size_t>(r_) + 1, 1);
    for (int k = 0; k < r_; ++k) {
      const Type* t = dropDims(target, k);
      dim_[k] = (t != nullptr && t->isArray() && t->len > 0) ? t->len : 1;
    }
    // ⚠️ `elemSpanAt(target,k)` 给的是"第 k 层对象的**子对象**"大小 = span[k+1]；
    //    差一层就让整张期望值表错位（S04 实测踩过两次）。这里显式错开一位。
    span_.assign(static_cast<size_t>(r_) + 1, 1);
    span_[0] = total_;
    for (int k = 1; k <= r_; ++k) span_[k] = elemSpanAt(target, k - 1);
    span_[r_] = 1;   // 一个标量占 1 格
  }

  // 【后置】把初始化器树展平进 out_（out_ 已分配、`expr` 全为空）。
  // 【前置】root 是花括号组、1 <= r_ <= 7（裸表达式与非法秩由 flattenInto 挡掉）。
  void run(const InitVal* root) {
    stack_.clear();
    push(root, 0, 0, 0, false);
    while (!stack_.empty()) {
      const size_t ti = stack_.size() - 1;
      const Frame cur = stack_[ti];      // 取拷贝：push_back 会让引用失效
      if (cur.src == nullptr || cur.i >= cur.src->list.size() ||
          cur.e >= dim_[cur.level]) {
        // 本对象填满（或流用尽）⇒ 弹栈，把"消费到哪儿"回填给父帧。
        stack_.pop_back();
        if (stack_.empty()) continue;
        Frame& p = stack_.back();
        if (cur.implicit) p.i = cur.i;   // 借用：父帧采纳子帧用掉的流位置
        else p.i += 1;                   // 花括号组 = 流里的一个孩子
        p.e += 1;
        continue;
      }
      const InitVal* c = cur.src->list[cur.i].get();
      const int nl = cur.level + 1;
      const int64_t nb = cur.base + cur.e * span_[nl];
      if (c == nullptr) {                // 残缺树（Sema 报错后仍走到这里）
        stack_[ti].i += 1;
        continue;
      }
      if (c->expr != nullptr) {
        if (nl == r_) {                  // 目标是标量：写在游标处，然后前进一格
          write(nb, c->expr.get());
          stack_[ti].i += 1;
          stack_[ti].e += 1;
        } else {                         // 借用：拿外层剩余的孩子填满子聚合
          // ★ 这里**不**推进父帧的 i/e —— 子帧弹栈时统一回填（借用 ⇒ 采纳
          //   子帧的 i；花括号 ⇒ i++、e++）。推进两次会让 `{{1,2},{3,4}}`
          //   只填完第一行就以为整个数组填满了。
          push(cur.src, nl, nb, cur.i, true);
        }
        continue;
      }
      if (nl == r_) {                    // 花括号作用在标量位置上（本组就此用完）
        assignScalar(c, nb);
        stack_[ti].i += 1;
        stack_[ti].e += 1;
        continue;
      }
      push(c, nl, nb, 0, false);         // 花括号作用在聚合子对象上
    }
  }

 private:
  struct Frame {
    const InitVal* src = nullptr;   // 孩子流 = src->list
    int level = 0;                  // 本对象所在层
    int64_t base = 0;               // 本对象起点（扁平下标）
    int64_t e = 0;                  // 对象内"下一个要填的子对象"下标
    size_t i = 0;                   // 流里"下一个孩子"下标
    bool implicit = false;          // true = 借用（没有花括号）
  };

  void push(const InitVal* src, int level, int64_t base, size_t i, bool implicit) {
    Frame f;
    f.src = src;
    f.level = level;
    f.base = base;
    f.i = i;
    f.implicit = implicit;
    stack_.push_back(f);
  }
  // 越界写一律忽略（Sema 已报 E-INIT-SHAPE）。
  // ★ 输出是**稀疏表**：只追加"被显式写到的位置"（见 FlatSlot 的注释）。
  void write(int64_t p, const Expr* e) {
    if (p >= 0 && p < total_) out_.push_back(FlatSlot{p, e});
  }
  // 花括号组作用在**标量**对象上：只认第一个孩子；首孩子还是组就继续剥
  // （while，不递归）。`{}` ⇒ 什么都不写（隐式 0）。
  void assignScalar(const InitVal* g, int64_t p) {
    while (g != nullptr) {
      if (g->list.empty()) return;
      const InitVal* first = g->list[0].get();
      if (first == nullptr) return;
      if (first->expr != nullptr) {
        write(p, first->expr.get());
        return;
      }
      g = first;
    }
  }

  FlatSlots& out_;
  int r_ = 0;
  int64_t total_ = 0;
  std::vector<int64_t> span_;   // 大小 r_+1（见构造函数：不设人为秩上限）
  std::vector<int64_t> dim_;    // 大小 r_+1
  std::vector<Frame> stack_;
};

}  // namespace

void flattenInto(const InitVal* root, const Type* target, FlatSlots& out) {
  out.clear();
  if (root == nullptr || target == nullptr) return;
  const int r = rank(target);
  const int64_t total = elementCount(target);
  if (total <= 0) return;
  // ★ **不**按元素数分配（见 FlatSlot 的注释）：只记被写到的位置。

  // ★ 标量可以带花括号（`int x = {1};`）——与 SemaDecl.cpp 的 doInitRoot 同一条规则
  //   （C11 §6.7.9：标量加花括号合法，多于一个元素才非法）。剥到最内层单元素；
  //   空组 `{}` ⇒ 全零。⚠️ 早先没剥，`= {1}` 被当成全零，值被悄悄丢掉。
  if (r == 0) {
    const InitVal* inner = root;
    while (inner->expr == nullptr && inner->list.size() == 1) inner = inner->list[0].get();
    if (inner->expr != nullptr) out.push_back(FlatSlot{0, inner->expr.get()});
    return;
  }

  // 数组对象的初始化器**必须**是花括号组（`a[4] = 4` 由 S03 报 E-INIT-SHAPE）。
  // 若树形不完整（Sema 报错后仍走到这里），保守地当成"标量写一格"。
  if (root->expr != nullptr) {
    out.push_back(FlatSlot{0, root->expr.get()});
    return;
  }
  if (r <= 0) return;   // 标量由上面的分支处理
  // ★ 这里**没有**秩上限：Flattener 的容量随秩走（合法 SysY 没有秩上限）。
  Flattener fl(target, out);
  fl.run(root);
  // 排序：`--emit=initplan` 的 `:data` 契约要求偏移升序，而借用的下钻可能
  // 让"写"的顺序不是下标升序（跨行的借用会先写后面的位置）。
  std::stable_sort(out.begin(), out.end(),
                   [](const FlatSlot& a, const FlatSlot& b) { return a.index < b.index; });
}

// ============================================================================
// 二 + 三、槽 → 值 → 计划
//
//   ★ 常量求值**只有一份实现**（ConstEvaluator，S03 交付的权威）：
//     字面量进制解析、`+ - * / %` 的折叠、`/0` 与 `INT_MIN/-1` 的归一化
//     （normDivInt / normRemInt / satFptosi）全部复用。本文件**不解析任何字面量**。
//
//   `eval()` 传的编号只在**真**报诊断时才用得上：局部初始化器里的 `i`（变量）、
//   `f(x)`（函数调用）**不是错误**，它们只是"不是常量表达式"⇒ 应当产出
//   `StoreExpr`。若沿用会报错的用法，合法的局部初始化器会刷出一堆
//   E-CONST-INIT，轨 A 立刻全红。所以本模块用一个**只用于求值、不打印**的
//   `DiagnosticEngine`（它的 `printAll` 从未被调用，见 §四 的说明）。
// ============================================================================
class Lowerer {
 public:
  Lowerer(const ConstEnv& env, InitPlan& plan) : eval_(env, scratchDiag_), plan_(plan) {}

  // ── 一个全局变量 ────────────────────────────────────────────────────────
  void lowerGlobal(const VarDef& v) {
    InitPlan::GlobalData g;
    g.name = v.name;
    g.loc = v.loc;
    if (v.semType != nullptr) g.type = *v.semType;
    g.allZero = true;

    const int64_t n = elementCount(v.semType);
    if (v.init != nullptr && n > 0) {
      flattenInto(v.init.get(), v.semType, slots_);
      // 只遍历**被写到的位置**（稀疏表已按下标升序）：没写到的就是 0，不进 :data。
      for (const FlatSlot& s : slots_) {
        if (s.expr == nullptr || s.index < 0 || s.index >= n) continue;
        ConstValue cv;
        if (!evalQuiet(*s.expr, cv)) continue;            // 非常量：S03 已报 E-CONST-INIT
        applyElemType(cv, v.semType);
        if (cv.sameBits(ConstValue::ofInt(0))) continue;  // 显式 0 ⇒ 也不进 :data
        g.nonzero.emplace_back(byteOffsetOf(s.index), cv);
        g.allZero = false;
      }
      // ★ 非零表按偏移升序：稀疏表已按下标升序 ⇒ 偏移天然递增（§4.3 的契约）。
    }
    if (g.allZero) {
      g.nonzero.clear();
      g.nonzero.shrink_to_fit();
    }
    plan_.globals.push_back(std::move(g));
  }

  // ── 一个局部变量（`label` = `函数名/变量名`）─────────────────────────────
  void lowerLocal(const std::string& label, const VarDef& v) {
    InitPlan::LocalInit lo;
    lo.name = label;
    lo.loc = v.loc;
    if (v.semType != nullptr) lo.type = *v.semType;
    plan_.locals.push_back(std::move(lo));
    InitPlan::LocalInit& back = plan_.locals.back();

    // ★ 未写初始化器 ⇒ **零动作**（prompt §3.2：局部不写 = 值不确定，
    //   不插零填充 —— 那是 C 的语义，也是热循环里的性能命门）。
    if (v.init == nullptr) return;

    const int64_t n = elementCount(v.semType);
    if (n <= 0) return;
    flattenInto(v.init.get(), v.semType, slots_);
    emitLocalActions(back, v, n);
  }

 private:
  // 求值：成功 ⇒ true。失败不报诊断（原因见文件头）。
  bool evalQuiet(const Expr& e, ConstValue& out) { return eval_.eval(e, out, "E-CONST-INIT"); }

  // 把常量值对齐到数组元素类型（`float b[2] = {1,2}`：int → float）。
  // Sema 已经通过插 `Cast` 表达过这件事，ConstEvaluator 会算出转换后的值；
  // 这里只是**兜底**（Sema 报错后的残缺树）。
  void applyElemType(ConstValue& v, const Type* arrayType) {
    const Type* et = elementType(arrayType);
    if (et == nullptr) return;
    if (isInt(et) && v.isFloat) v = ConstValue::ofInt(satFptosi(v.f));
    else if (isFloat(et) && !v.isFloat) v = ConstValue::ofFloat(static_cast<float>(v.i));
  }

  // ── 局部动作生成（prompt §4.2 的规模策略表）──────────────────────────
  //   输入是**稀疏表**（只有被显式写到的位置，按下标升序）+ 元素总数 `n`。
  //   逐位置语义：
  //     * 没被写到的位置 ⇒ 常量 0（规范 §3 ConstDef 6.3：未写到的隐式初始化）
  //     * 被写到的位置   ⇒ 求值：成功 ⇒ 常量（可能与 0 同位）；失败 ⇒ 运行期表达式
  //   三态按顺序合并成动作：
  //     * 常量 0（"没写到"与"显式 0"同等对待）⇒ 攒进**零段**；
  //     * 连续的非零常量 ⇒ 攒进**常量段**（≥ kMemcpyMin 个 ⇒ 一条 MemcpyConst，
  //       否则逐个 StoreConst）；
  //     * 运行期表达式 ⇒ 先把零段与常量段 flush 掉，再落一条 StoreExpr
  //       —— 于是"动作的执行顺序 == 行主序游标顺序"是**构造性**成立的（§4.2）。
  //   ⇒ `int a[4096] = {1};` 产出 2 条动作；`int a[50000000] = {};` 产出 1 条
  //     Zero 且**不分配** 50M 个格子（S04 实测：稠密格子会吃 400 MB）。
  void emitLocalActions(InitPlan::LocalInit& lo, const VarDef& v, int64_t n) {
    std::vector<ConstValue> cbuf;
    int64_t cstart = 0;
    // 待落盘的零段：[zrun, zrun+zlen)。**可能横跨任意长的空洞**，所以必须
    // 用"区间端点"而不是"逐格累加"来表示 —— 否则 `int a[50000000] = {};`
    // 要循环 5000 万次（实测会慢到超时；虽然结果对，但那是白花）。
    int64_t zrun = -1;          // 零段起点（-1 = 没有）
    int64_t zend = -1;          // 零段终点（开区间）
    int64_t seen = 0;           // 已经处理过的位置数（= 下一个待处理的下标）

    auto flushZero = [&]() {
      if (zrun >= 0 && zend > zrun) {
        InitAction a;
        a.kind = InitActionKind::Zero;
        a.offset = byteOffsetOf(zrun);
        a.bytes = static_cast<uint64_t>(zend - zrun) * kScalarBytes;
        a.loc = v.loc;
        lo.actions.push_back(std::move(a));
      }
      zrun = -1;
      zend = -1;
    };
    auto flushCbuf = [&]() {
      if (cbuf.empty()) return;
      const int64_t start = cstart;
      InitAction a;
      a.loc = v.loc;
      if (cbuf.size() >= kMemcpyMin) {
        a.kind = InitActionKind::MemcpyConst;
        a.offset = byteOffsetOf(start);
        a.values = std::move(cbuf);
      } else if (cbuf.size() == 1) {
        a.kind = InitActionKind::StoreConst;
        a.offset = byteOffsetOf(start);
        a.value = cbuf[0];
      } else {
        // 2..3 个元素：逐个 StoreConst（§4.2："连续常量段 < 4 ⇒ 逐个"）。
        // ⚠️ 这里**故意**与 §4.3 的示例 `(MemcpyConst 8 :int 3 4 5)` 不一致：
        //    那张表是可执行的判据、示例只是格式说明；两者冲突时以表为准。
        for (size_t i = 0; i < cbuf.size(); ++i) {
          InitAction one;
          one.kind = InitActionKind::StoreConst;
          one.offset = byteOffsetOf(start + static_cast<int64_t>(i));
          one.value = cbuf[i];
          one.loc = v.loc;
          lo.actions.push_back(std::move(one));
        }
        cbuf.clear();
        return;
      }
      cbuf.clear();
      lo.actions.push_back(std::move(a));
    };
    // 把 [from, to) 这整段"没写到"的位置并入零段（O(1) 摊还）。
    auto addZeroRun = [&](int64_t from, int64_t to) {
      if (to <= from) return;
      flushCbuf();
      if (zrun < 0) zrun = from;
      if (to > zend) zend = to;
    };
    // 处理一个"被写到的位置"。
    auto addWritten = [&](int64_t i, const Expr* e) {
      ConstValue cv;
      bool isConst = false;
      if (e != nullptr && evalQuiet(*e, cv)) {
        applyElemType(cv, v.semType);
        isConst = true;
      }
      if (isConst && cv.sameBits(ConstValue::ofInt(0))) {
        addZeroRun(i, i + 1);          // 显式 0 与"没写到"同等对待
        return;
      }
      flushZero();
      if (isConst) {
        if (cbuf.empty()) cstart = i;
        cbuf.push_back(cv);
        return;
      }
      flushCbuf();
      InitAction a;
      a.kind = InitActionKind::StoreExpr;
      a.offset = byteOffsetOf(i);
      a.expr = e;
      a.loc = (e != nullptr) ? e->loc : v.loc;
      lo.actions.push_back(std::move(a));
    };

    for (const FlatSlot& s : slots_) {
      if (s.index < 0 || s.index >= n) continue;
      addZeroRun(seen, s.index);       // 两格之间 = 没写到 = 0（O(1) 摊还）
      addWritten(s.index, s.expr);
      seen = s.index + 1;
    }
    addZeroRun(seen, n);               // 尾部（`int a[4096] = {1}` 的 4095 个零）
    flushZero();
    flushCbuf();
  }

  // 求值用的诊断出口：**只收集、从不打印**。为什么要一个真的引擎而不是
  // "忽略返回值"：`ConstEvaluator` 的接口就是"失败 ⇒ 报一条诊断"，本模块
  // 借用它做"是不是常量"的判定，报出来的东西一个都用不上。
  DiagnosticEngine scratchDiag_;
  ConstEvaluator eval_;
  InitPlan& plan_;
  FlatSlots slots_;
};

// ============================================================================
// 四、遍历（显式工作栈）
//
//   只需要走两件事：**顶层/块里的 Decl**（找 VarDef）与**函数定义**（决定
//   `Global` 还是 `函数名/变量名`）。语句只作为"可能包着 Block"的容器。
//   ★ 不递归：4000 层嵌套块的病态输入（prompt §九）在这里不占调用栈。
// ============================================================================
struct Frame {
  const Node* node = nullptr;
  size_t idx = 0;
  bool block = false;      // true ⇒ items 是 BlockStmt::items
  bool resetFunc = false;  // true ⇒ 清掉"当前函数"上下文（函数体走完了）
};

class Walker {
 public:
  Walker(const ConstEnv& env, InitPlan& plan) : lower_(env, plan) {}

  void run(const CompUnit& unit) {
    std::vector<Frame> st;
    st.push_back(Frame{&unit, 0, false});
    while (!st.empty()) {
      const Frame f = st.back();
      st.pop_back();
      if (f.resetFunc) { funcName_.clear(); continue; }   // 函数体走完
      if (f.node == nullptr) continue;
      if (f.block) { stepBlock(f, st); continue; }
      stepNode(f, st);
    }
  }

 private:
  void stepNode(const Frame& f, std::vector<Frame>& st) {
    const std::string_view k = f.node->nodeKind();
    if (k == "CompUnit") { stepItems(static_cast<const CompUnit*>(f.node)->items, f, st, false); return; }
    if (k == "FuncDef") {
      const auto* fd = static_cast<const FuncDef*>(f.node);
      funcName_ = fd->name;
      if (fd->body != nullptr) {
        // ★ 函数体处理完必须清掉 `funcName_`，否则**下一个**顶层 `Decl` 会被当成
        //   "上一个函数的局部变量"。实测症状：`int f(){…} int b[…] = {};` 里 `b`
        //   变成 `(Local f/b …)`，转储里根本没有 `Global`，而 490 个用例全绿。
        st.push_back(Frame{f.node, 0, false, /*resetFunc=*/true});
        st.push_back(Frame{fd->body.get(), 0, true});
      } else {
        funcName_.clear();
      }
      return;
    }
    if (k == "BlockStmt") { st.push_back(Frame{f.node, 0, true}); return; }
    if (k == "IfStmt") {
      const auto* s = static_cast<const IfStmt*>(f.node);
      if (s->elseS != nullptr) st.push_back(Frame{s->elseS.get(), 0, false});
      if (s->thenS != nullptr) st.push_back(Frame{s->thenS.get(), 0, false});
      return;
    }
    if (k == "WhileStmt") {
      const auto* s = static_cast<const WhileStmt*>(f.node);
      if (s->body != nullptr) st.push_back(Frame{s->body.get(), 0, false});
      return;
    }
  }

  void stepItems(const std::vector<std::unique_ptr<Node>>& items, const Frame& f,
                 std::vector<Frame>& st, bool block) {
    if (f.idx >= items.size()) return;
    // ⚠️ 回边帧必须**保持 block 标志**：写成 false 会让同一个块被反复当"节点"
    //    打开 ⇒ 无限循环（实测踩过：一个 `int main(){…}` 就死循环）。
    st.push_back(Frame{f.node, f.idx + 1, block});
    const Node* item = items[f.idx].get();
    if (item == nullptr) return;
    // ★ 只有 Decl 会产生初始化动作；函数定义/语句交给 stepNode。
    if (item->nodeKind() == std::string_view("Decl")) {
      lowerDecl(*static_cast<const Decl*>(item));
      return;
    }
    stepNode(Frame{item, 0, false}, st);
  }

  void stepBlock(const Frame& f, std::vector<Frame>& st) {
    const auto* b = static_cast<const BlockStmt*>(f.node);
    stepItems(b->items, f, st, true);
  }

  // Decl：全局在上（funcName_ 为空），局部用 `函数名/变量名`。
  void lowerDecl(const Decl& d) {
    const bool global = funcName_.empty();
    for (const std::unique_ptr<VarDef>& vp : d.defs) {
      if (vp == nullptr) continue;
      const VarDef& v = *vp;
      if (v.semType == nullptr) continue;   // Sema 没能给出类型（已报错）⇒ 跳过
      if (global) lower_.lowerGlobal(v);
      else lower_.lowerLocal(funcName_ + "/" + v.name, v);
    }
  }

  Lowerer lower_;
  std::string funcName_;
};

}  // namespace

// ============================================================================
// 对外入口
// ============================================================================
InitPlan runInitLowering(const CompUnit& unit, const ConstEnv& env) {
  InitPlan plan;
  Walker w(env, plan);
  w.run(unit);
  return plan;
}

std::string initPlanSummary(const InitPlan& plan) {
  std::string out;
  out += "globals=" + std::to_string(plan.globals.size()) +
         " locals=" + std::to_string(plan.locals.size()) + "\n";
  for (const InitPlan::GlobalData& g : plan.globals) {
    out += "G " + g.name + " : " + typeText(&g.type) +
           (g.allZero ? " :zero" : " :data(" + std::to_string(g.nonzero.size()) + ")") + "\n";
  }
  for (const InitPlan::LocalInit& l : plan.locals) {
    out += "L " + l.name + " : " + typeText(&l.type) + " : actions=" +
           std::to_string(l.actions.size()) + "\n";
  }
  return out;
}

}  // namespace sysy
