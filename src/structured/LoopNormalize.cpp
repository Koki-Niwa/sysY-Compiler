// ============================================================================
// LoopNormalize.cpp —— `while` → `for` 的规范化（S05b 交付物 #1；规格见
// `docs/handoff/03-设计/结构化IR设计.md` §1.3 / §3.1① 与 S05b prompt §三）
//
//   处理顺序**不能反**（§3.1，本文件的函数顺序就是它）：
//     ① `desugarContinues()`  —— `continue` 消解
//     ② `findIv()`            —— IV 识别
//     ③ `checkBound()`        —— 边界归一化 + 参数符号提升
//     ④ `analyzeStep()`       —— 每条路径自增证明
//     ⑤ `processLoop()`       —— 构造 `ForOp`
//   判定用的只读工具在 `LoopAnalysis.cpp`；本文件负责**改写**。
//
// ── ★ 两趟结构：干跑 + 真跑（"零改动"判据的前提）─────────────────────────
//   prompt §六 轨 C.3 要求"一个循环都没规范化的文件，`--normalize` 前后 dump
//   **逐字节相同**"。而 `continue` 消解会改写体。若"先消解 → 判定失败 → 保留
//   `WhileOp`"，那个文件的 dump 就变了 ⇒ 判据假红。
//   ⇒ 对每个循环跑**两遍同一个分析器**：第一遍 `dry = true`（只判定、不改树），
//     通过后才跑第二遍 `dry = false`（真改写）。判定逻辑**一份代码**共用
//     （两套判定会漂移 —— S03/S04 的教训）。
//
// ── ★ `continue` 消解的规范形式（本关唯一"没有唯一答案"的地方）──────────
//   轨 D 是"独立实现的第二份规范化器"，它按 prompt §三 自己推导这个形式。
//   必须写下来，否则两份实现会在"用 then 还是 else 装剩余体"上分叉。
//
//   **规范形式（字面照 prompt §3.2）：`continue` 归一化为 `if (!cond) { B }`。**
//   我们的落地（`IfOp` 的两个 Region 是 then/else）：
//     `[ …, (Break), S1..Sk ]`  →  `[ …, (If (Eq C 0)) { (Yield) } { S1..Sk } ]`
//   即 **剩余体进 `else`，`then` 只留 `(Yield)`**（满足 I4：每个 Region 恰好
//   一个终结 Op）。三条必须与轨 D 对齐的细节：
//     ① 反转条件用 **`Eq C 0`**（新发射一条 `Int 0`），**不是**交换比较谓词
//        —— 后者要理解 `Lt`/`Le`/… 的补运算，属于算术改写（§十一.3 禁止）；
//        `C == 0` ⇔ `C` 为假 ⇒ 走 `then`（空的 `Yield`）⇒ 等价于 `continue`。
//     ② 这里的 `(Break)` 是"**后面还有语句**"的那个 `BreakOp`（`break` 会让
//        后面的语句不可达，IRGen 不会生成它们 ⇒ 它必是 `continue`）。
//     ③ 若 `(Break)` 已经是**体的最后一行** ⇒ 它是"尾部 continue"
//        （`i = i + 1; continue;`）⇒ **不新建 `IfOp`**，由 `applyStep` 换成
//        `(Yield)`。若它在**体里某个 `IfOp` 分支的末尾** ⇒ 也**不包装**
//        （那个 `IfOp` 的另一个分支保证"条件为假时继续执行体的其余部分"），
//        由 `applyStep` 把那个 `BreakOp` 换成 `(Yield)`。
//
// ── `break` 的处置（prompt §3.3）──────────────────────────────────────────
//   设计文档 I3 要求 `ForOp` 体内没有 `break` ⇒ **含 `break` 的 `while`
//   一律不规范化**（第一版不留"可数 break"的口子）。判据见
//   `LoopAnalysis.h` 的"`BreakOp` 的双重身份"。
//
// ── 失败怎么办（安全降级）─────────────────────────────────────────────────
//   **保留 `WhileOp`、不报诊断**；但"没规范化"必须**可观测**
//   （`--dump-loopnorm-stats` 打印原因直方图）——"静默不规范化"和"静默截断"
//   是同一类问题（S02/S04 各栽过一次）。
// ============================================================================
#include "structured/LoopNormalize.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "structured/LoopAnalysis.h"
#include "structured/LoopRewrite.h"

namespace sysy {
namespace sir {
namespace {

const char* const kReasonNames[] = {
    "条件不匹配（不是 Lt(iv, bound)）",
    "IV 未识别（条件左侧不是标量槽的 Load）",
    "非仿射边界（含 Call/Store 或边界依赖 IV）",
    "边界在体内被写（提到循环外会改变语义）",
    "路径自增证明失败（体内 IV 不是每条路径 +1）",
    "体内对 IV 槽有未建模的访问",
    "体内含 break",
    "体内含 Call（边界是否被写证不出来）",
};
static_assert(sizeof(kReasonNames) / sizeof(kReasonNames[0]) ==
                  static_cast<size_t>(SkipReason::Count),
              "原因名表必须与 SkipReason 一一对应");

struct LoopSite {
  Op* loop = nullptr;
  const Op* fn = nullptr;
  std::string funcName;
};

void collectLoops(const Op* module, std::vector<LoopSite>& out) {
  struct Item { const Op* op; const Op* fn; std::string name; };
  std::vector<Item> stack;
  const Region* modR = module->region(0);
  if (modR != nullptr) {
    for (size_t i = modR->size(); i-- > 0;) {
      const Op* op = modR->at(i);
      if (op != nullptr) stack.push_back(Item{op, nullptr, std::string()});
    }
  }
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    const Op* op = it.op;
    if (op == nullptr) continue;
    const Op* fn = it.fn;
    std::string fnName = it.name;
    if (op->kind == OpKind::Func) { fn = op; fnName = op->strAttr(0); }
    if (op->kind == OpKind::While) {
      LoopSite site;
      site.loop = const_cast<Op*>(op);
      site.fn = fn;
      site.funcName = fnName;
      out.push_back(site);
    }
    for (size_t i = op->numRegions(); i-- > 0;) {
      const Region* r = op->region(i);
      if (r == nullptr) continue;
      for (size_t j = r->size(); j-- > 0;) {
        const Op* sub = r->at(j);
        if (sub != nullptr) stack.push_back(Item{sub, fn, fnName});
      }
    }
  }
}

void processLoop(Op* loop, const Op* fnOp, const ParentMap& par, Arena& arena,
                 LoopNormStats& stats, const std::string& funcName) {
  ++stats.whileSeen;
  const auto it = par.find(loop);
  if (it == par.end() || it->second.second == nullptr) {
    stats.noteSkip(funcName, loop->loc, SkipReason::CondNotLt);
    return;
  }
  Region* parent = const_cast<Region*>(it->second.second);
  Region* condR = loop->region(0);
  Region* bodyR = loop->region(1);

  // ★ 判定**只读** ⇒ 判定失败时这个循环**逐字节零改动**
  //   （prompt §六 轨 C.3 的"不触发用例零改动"因此天然成立，不需要回滚）。
  const detail::RewriteResult rr = detail::analyzeAndRewriteLoop(loop, arena);
  if (!rr.ok) {
    stats.noteSkip(funcName, loop->loc, rr.fail);
    return;
  }

  // ── ⑤ 构造 `ForOp`（条件 Region 的边界表达式搬到循环之前）──────────────
  const std::vector<Op*> condOps = condCandidates(condR);
  const Value iv = rr.iv;
  const Value upper = rr.upper;
  const std::string ivName = dumpResultName(fnOp, rr.ivLoad);

  std::vector<Op*> pv = parent->ops();
  size_t at = pv.size();
  for (size_t i = 0; i < pv.size(); ++i) {
    if (pv[i] == loop) { at = i; break; }
  }
  Op* lower = arena.makeOp(OpKind::Load, loop->loc);
  lower->addAttr(Attr::ofType(typePool().i32()));
  lower->addOperand(iv);
  Value lowerV = lower->addResult(typePool().i32());
  Op* stepOp = arena.makeOp(OpKind::Int, loop->loc);
  stepOp->addAttr(Attr::ofInt(1));
  Value stepV = stepOp->addResult(typePool().i32());

  Op* forOp = arena.makeOp(OpKind::For, loop->loc);
  forOp->addAttr(Attr::ofStr(ivName));
  forOp->addOperand(iv);        // 操作数 0：IV 的槽（身份；I1 判定用）
  forOp->addOperand(lowerV);    // 操作数 1：lower（**进入循环时的 IV 值**）
  forOp->addOperand(upper);     // 操作数 2：upper（边界表达式的结果）
  forOp->addOperand(stepV);     // 操作数 3：step（恒为 1）
  forOp->addRegion(bodyR);

  std::vector<Op*> rebuilt;
  rebuilt.reserve(pv.size() + condOps.size() + 3);
  for (size_t i = 0; i < pv.size(); ++i) {
    if (i == at) {
      for (Op* o : condOps) rebuilt.push_back(o);
      rebuilt.push_back(lower);
      rebuilt.push_back(stepOp);
    }
    if (pv[i] == loop) { rebuilt.push_back(forOp); continue; }
    rebuilt.push_back(pv[i]);
  }
  parent->replaceAll(rebuilt);

  ++stats.forBuilt;
  stats.continuesResolved += rr.continuesResolved;
  stats.tailContinues += rr.tailContinues;
}

}  // namespace

// ============================================================================
// 公共接口
// ============================================================================
const char* skipReasonName(SkipReason r) {
  const size_t i = static_cast<size_t>(r);
  return i < static_cast<size_t>(SkipReason::Count) ? kReasonNames[i] : "?";
}

void LoopNormStats::noteSkip(const std::string& fn, SourceLoc loc, SkipReason r) {
  const size_t i = static_cast<size_t>(r);
  if (i < static_cast<size_t>(SkipReason::Count)) ++skip[i];
  Detail d;
  d.func = fn;
  d.line = loc.line;
  d.reason = r;
  skipped.push_back(std::move(d));
}

Op* normalizeLoops(Op* module, Arena& arena, LoopNormStats& stats) {
  if (module == nullptr || module->kind != OpKind::Module) return module;
  const ParentMap par = buildParents(module);
  std::vector<LoopSite> sites;
  collectLoops(module, sites);   // 先序
  // ★ **内层先处理**（把先序反过来）：`collectLoops` 是先序（父先于子），
  //   反过来保证"每个后代都先于它的祖先"。
  //   为什么顺序重要（实测的真 bug，被"带开关的往返"抓出来）：
  //   内层规范化会把 `WhileOp` 变成 `ForOp`，于是**外层的体形状变了** ——
  //   外层"看到的是 While 还是 For"直接影响它的判定（嵌套循环里的
  //   `BreakOp`/自增怎么算）。若外层先判、内层后改，则
  //   `run(run(X)) != run(X)`：第二次运行时外层看到的是已经变成 `For` 的内层，
  //   判定结果与第一次不同（实测 `transpose2.sy` 第一次没规范化、第二次规范化了）。
  //   ⇒ 由内到外，两次运行看到的内层形状一致。
  for (size_t i = sites.size(); i-- > 0;) {
    processLoop(sites[i].loop, sites[i].fn, par, arena, stats, sites[i].funcName);
  }
  return module;
}

std::string formatLoopNormStats(const LoopNormStats& s) {
  std::string out;
  out += "== loopnorm stats ==\n";
  out += "while-seen " + std::to_string(s.whileSeen) + "\n";
  out += "for-built " + std::to_string(s.forBuilt) + "\n";
  out += "kept-while " + std::to_string(s.whileSeen - s.forBuilt) + "\n";
  out += "continues-resolved " + std::to_string(s.continuesResolved) + "\n";
  out += "  of-which-tail " + std::to_string(s.tailContinues) + "\n";
  out += "-- skip histogram（未规范化原因）--\n";
  for (size_t i = 0; i < static_cast<size_t>(SkipReason::Count); ++i) {
    out += "skip ";
    out += skipReasonName(static_cast<SkipReason>(i));
    out += " = " + std::to_string(s.skip[i]) + "\n";
  }
  out += "-- details（逐条：函数 / 行 / 原因）--\n";
  for (const auto& d : s.skipped) {
    out += "kept " + d.func + " :" + std::to_string(d.line) + " " +
           skipReasonName(d.reason) + "\n";
  }
  return out;
}

}  // namespace sir
}  // namespace sysy
