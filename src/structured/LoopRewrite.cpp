// ============================================================================
// LoopRewrite.cpp —— 判定与改写的实现（规格见 LoopRewrite.h）
// ============================================================================

#include "structured/LoopRewrite.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "structured/LoopAnalysis.h"

namespace sysy {
namespace sir {
namespace detail {
namespace {

// ============================================================================
// Analyzer —— 一次规范化的全部判定（`dry` 决定是否真的改写）
// ============================================================================
class Analyzer {
 public:
  // 【前置】`loop` 是待判定的 `WhileOp`，`body` 是它的体 Region。
  // 【后置】构造后先 `analyzeOnce()`（**只读**），成功才 `applyStep()`（改写）。
  Analyzer(Arena& a, Op* loop, Region* body) : arena_(a), loop_(loop), body_(body) {}

  // ── ① `continue` 消解 ───────────────────────────────────────────────────
  //   ★ 规范形式（prompt §3.2 的字面）：`continue` 归一化为 `if (!cond) { B }`
//   …（原有 39 行说明，已并入 S06 报告）
  //   （`Break` → `Yield`）⇒ `return i` 从 5 变成 8。实测就是这个。
  static bool hasRealStmtAfter(const Region* r, size_t i) {
    if (r == nullptr) return false;
    for (size_t k = i + 1; k < r->size(); ++k) {
      const Op* op = r->at(k);
      if (op == nullptr) continue;
      if (op->kind == OpKind::Yield || op->kind == OpKind::Break) continue;
      return true;
    }
    return false;
  }

  bool checkContinues(const Region* body) const {
    if (body == nullptr) return true;
    for (size_t i = 0; i < body->size(); ++i) {
      const Op* op = body->at(i);
      if (op == nullptr) continue;
      // ★★ S06 修正之后，`continue` 与 `break` 是**两个不同的 Op**
      //   （`ContinueOp` / `BreakOp`，见 StructuredIR.h 的说明）⇒ 这里不再需要
//   …（原有 6 行说明，已并入 S06 报告）
      //   ⚠️ 旧实现在这里猜，而两个程序的结构化 IR **逐字节相同** ⇒ 猜不出来。
      if (op->kind == OpKind::Break) return false;
      if (op->kind == OpKind::Continue) continue;   // 尾部 continue：允许
      if (op->kind == OpKind::While || op->kind == OpKind::For) {
        // 内层循环自己管；但内层若还留着 Break/Continue（= 它没能规范化），
        //   本层也一律不规范化（否则 `ForOp` 体内会有 Break/Continue，违反 I3）。
        if (regionHasBreak(op->region(1)) || regionHasContinue(op->region(1))) return false;
        continue;
      }
      if (op->kind != OpKind::If) continue;
      const Region* a = op->region(0);
      const Region* b = op->region(1);
      const bool ac = a != nullptr && !a->empty() && a->back() != nullptr &&
                      a->back()->kind == OpKind::Continue;
      const bool bc = b != nullptr && !b->empty() && b->back() != nullptr &&
                      b->back()->kind == OpKind::Continue;
      const bool abrk = a != nullptr && !a->empty() && a->back() != nullptr &&
                        a->back()->kind == OpKind::Break;
      const bool bbrk = b != nullptr && !b->empty() && b->back() != nullptr &&
                        b->back()->kind == OpKind::Break;
      // 分支末尾的**真 `break`** ⇒ 拒绝（两条路径都会跳出循环之外）
      if (abrk || bbrk) return false;
      if (ac && bc) return false;               // 两条路径都 continue：不处理（无收益）
      if (ac || bc) continue;                   // 形态 A/B：允许，改写时包装
      // 分支里没有"末尾 continue" ⇒ 若有别的 Break/Continue（非末尾）⇒ 保守拒绝
      if (regionHasBreak(a) || regionHasBreak(b)) return false;
      if (regionHasContinue(a) || regionHasContinue(b)) return false;
    }
    return true;
  }

  // 【后置】把体的 continue 按上面的规范形式改写（**只在判定通过后调用**）。
  void resolveContinues() {
    // 先处理"体尾的尾部 continue"：换成 `(Yield)`
    std::vector<Op*> v = body_->ops();
    if (!v.empty() && v.back() != nullptr && v.back()->kind == OpKind::Continue) {
      Op* y = mk(OpKind::Yield, v.back()->loc);
      v.back() = y;
      body_->replaceAll(v);
      ++tailContinues_;
    }
    // 从**后往前**处理体顶层的 `If`（保证包装的嵌套顺序）
    for (;;) {
      std::vector<Op*> cur = body_->ops();
      size_t k = cur.size();
      Op* target = nullptr;
      size_t which = 0;
      for (size_t i = cur.size(); i-- > 0;) {
        Op* op = cur[i];
        if (op == nullptr || op->kind != OpKind::If) continue;
        Region* r0 = op->region(0);
        Region* r1 = op->region(1);
        const bool ab = r0 != nullptr && !r0->empty() && r0->back() != nullptr &&
                        r0->back()->kind == OpKind::Continue;
        const bool bb = r1 != nullptr && !r1->empty() && r1->back() != nullptr &&
                        r1->back()->kind == OpKind::Continue;
        if (!ab && !bb) continue;
        target = op;
        k = i;
        which = ab ? 0 : 1;
        break;
      }
      if (target == nullptr) break;
      // ① 该分支末尾的 `Break` → `(Yield)`
      Region* br = target->region(which);
      std::vector<Op*> bw = br->ops();
      Op* y = mk(OpKind::Yield, bw.back()->loc);
      bw.back() = y;
      br->replaceAll(bw);
      ++tailContinues_;
      // ② 体里 `If` 之后的语句（REST）：非空 ⇒ 包进 `If(!C){Yield}{REST}`
      std::vector<Op*> rest(cur.begin() + static_cast<long>(k) + 1, cur.end());
      if (rest.empty()) break;                     // 后面没有语句：无需包装
      // ★ **规范形式 = `if (!C) { REST }`** ⇒ `REST` 进 **then**、`else` 只留 `(Yield)`。
      //   ⚠️ 这里踩过一个**只有轨 E 抓得到**的坑：先把 `REST` 放进 `else`、`then` 留
//   …（原有 4 行说明，已并入 S06 报告）
      //   `If(Eq C 0) { REST } { (Yield) }`。
      Region* thenR = mkRegion();
      for (Op* o : rest) thenR->push(o);
      resolveRegionTail(thenR);                    // REST 里可能还有 continue
      Region* elseR = mkRegion();
      elseR->push(mk(OpKind::Yield, target->loc));
      // `!C` = `Eq C 0`（新发射 `Int 0`）
      Op* zero = mk(OpKind::Int, target->loc);
      zero->addAttr(Attr::ofInt(0));
      Value zeroV = zero->addResult(typePool().i32());
      Op* neg = mk(OpKind::Eq, target->loc);
      neg->addOperand(target->numOperands() >= 1 ? target->operand(0) : nullptr);
      neg->addOperand(zeroV);
      Value negV = neg->addResult(typePool().i32());
      Op* wrapper = mk(OpKind::If, target->loc);
      wrapper->addOperand(negV);
      wrapper->addRegion(thenR);
      wrapper->addRegion(elseR);
      std::vector<Op*> rebuilt(cur.begin(), cur.begin() + static_cast<long>(k) + 1);
      rebuilt.push_back(zero);
      rebuilt.push_back(neg);
      rebuilt.push_back(wrapper);
      body_->replaceAll(rebuilt);
      ++continuesResolved_;
    }
    // ★ 包装之后体的最后一条语句是 `IfOp`（不是终结符）⇒ 补一个 `(Yield)`
    //   （I4：每个 Region 恰好一个终结 Op 且在最后一行）。
    //   ⚠️ 漏掉这一步会让**每一个**含分支末尾 continue 的循环在规范化后
    //      违反 I4（实测：`transpose0.sy` 等 14 个文件报"Region 没有终结 Op"）。
    std::vector<Op*> fin = body_->ops();
    if (fin.empty() || fin.back() == nullptr || !isTerminator(fin.back()->kind)) {
      fin.push_back(mk(OpKind::Yield, loop_->loc));
      body_->replaceAll(fin);
    }
  }

  // 处理"被搬进包装分支里的 REST"：只做"分支末尾 `Break` → `(Yield)`"这一层
  //   （REST 里若还有更复杂的 continue，`checkContinues` 已经保守挡掉了）。
  void resolveRegionTail(Region* r) {
    if (r == nullptr) return;
    std::vector<Op*> v = r->ops();
    for (Op* op : v) {
      if (op == nullptr || op->kind != OpKind::If) continue;
      for (size_t k = 0; k < op->numRegions(); ++k) {
        Region* sub = op->region(k);
        if (sub == nullptr || sub->empty()) continue;
        if (sub->back() != nullptr && sub->back()->kind == OpKind::Continue) {
          std::vector<Op*> w = sub->ops();
          w.back() = mk(OpKind::Yield, w.back()->loc);
          sub->replaceAll(w);
          ++tailContinues_;
        }
      }
    }
    // 尾部 `continue`（`ContinueOp` 在 Region 末尾）⇒ 换成 `(Yield)`。
    //   ⚠️ 这里**只认 `ContinueOp`**：真 `break` 是 `BreakOp`，`checkContinues`
    //      已经把含它的循环整条拒绝了，走到这里的不可能是 break。
    if (!v.empty() && v.back() != nullptr && v.back()->kind == OpKind::Continue) {
      v.back() = mk(OpKind::Yield, v.back()->loc);
      r->replaceAll(v);
      ++tailContinues_;
    }
  }

  // ── ② IV 识别 ───────────────────────────────────────────────────────────
  void findIv(const Region* condR) {
    if (condR == nullptr || condR->empty()) { give(SkipReason::CondNotLt); return; }
    const Op* term = condR->back();
    if (term == nullptr || term->kind != OpKind::Yield || term->numOperands() != 1) {
      give(SkipReason::CondNotLt);
      return;
    }
    const Op* cmp = term->operand(0) != nullptr ? term->operand(0)->definer : nullptr;
    if (cmp == nullptr || cmp->kind != OpKind::Lt || cmp->numOperands() != 2) {
      give(SkipReason::CondNotLt);
      return;
    }
    for (int side = 0; side < 2; ++side) {
      const Value v = cmp->operand(static_cast<size_t>(side));
      const Op* ld = (v != nullptr) ? v->definer : nullptr;
      if (ld == nullptr || ld->kind != OpKind::Load || ld->numOperands() != 1) continue;
      const Op* slot = (ld->operand(0) != nullptr) ? ld->operand(0)->definer : nullptr;
      if (slot == nullptr || slot->kind != OpKind::Alloca || slot->numResults() != 1) continue;
      const Attr* ta = slot->attr(0);
      const Type* ty = (ta != nullptr && ta->kind == Attr::Kind::Type) ? ta->ty : nullptr;
      if (ty == nullptr || ty->kind != TypeKind::I32) continue;
      ivSlot_ = const_cast<Result*>(slot->result(0));
      upper_ = cmp->operand(static_cast<size_t>(1 - side));
      ivLoad_ = ld;
      return;
    }
    give(SkipReason::IvNotIdentified);
  }

  // ── ③ 边界归一化 + 参数符号提升 ─────────────────────────────────────────
  void checkBound() {
    if (upper_ == nullptr) { give(SkipReason::NonAffineBound); return; }
    const std::unordered_set<const Op*> written = writtenSlotsIn(body_);
    if (!provePure(upper_, ivSlot_, written)) { give(SkipReason::NonAffineBound); return; }
    // 边界子树里出现的 Alloca 槽若在体内被写（经 GEP 的数组元素读）⇒ 拒绝
    std::unordered_set<const Op*> seen;
    std::vector<const Op*> st{upper_->definer};
    while (!st.empty()) {
      const Op* op = st.back();
      st.pop_back();
      if (op == nullptr || !seen.insert(op).second) continue;
      if (op->kind == OpKind::Alloca && written.count(op) != 0) {
        give(SkipReason::BoundWrittenInBody);
        return;
      }
      for (size_t i = 0; i < op->numOperands(); ++i) {
        const Value v = op->operand(i);
        st.push_back(v != nullptr ? v->definer : nullptr);
      }
    }
  }

  // ── ④ 每条路径自增证明 ──────────────────────────────────────────────────
  void analyzeStep() {
    if (hasRealBreak(body_)) { give(SkipReason::HasBreak); return; }
    const int direct = directPathIncrements(body_);
    if (direct != 1) { give(SkipReason::PathStepFail); return; }
    int contCount = 0;
    if (!continueBranchesHaveOne(body_, &contCount)) { give(SkipReason::PathStepFail); return; }
    // 体内对 IV 槽的**写**必须全部被建模为自增。
    //   ★ 只查写、不查读：`while (i<n) { s += i; i = i+1; }` 里 `s += i` 会
    //     `Load i`，而那个 `Load` **也在条件 Region 的 `Lt` 里**（同一结果被
    //     两处引用）。规范化后体内的自增被摘掉 ⇒ 体内那次读的"值"由 `ForOp`
    //     提供（同一个 IV），语义不变。**读不构成风险，写才构成风险**。
    bool bad = false;
    walkOps(body_, [&](const Op* op) {
      if (bad) return;
      if (op->kind == OpKind::Store && op->numOperands() == 2 && op->operand(1) == ivSlot_ &&
          !isIvIncrement(op, ivSlot_, nullptr)) {
        bad = true;
      }
    });
    if (bad) give(SkipReason::IvWrittenUnmodeled);
  }

  // 真 `break` 的判定（详见 LoopAnalysis.h 的"双重身份"）：
  //   * 顶层 `BreakOp` 后面还有语句 ⇒ 真 `break`（`break` 会让它们不可达）；
//   …（原有 23 行说明，已并入 S06 报告）
  //      `Break`），语义永远正确；判错成 continue 才会改语义。
  bool hasRealBreak(const Region* r) const {
    if (r == nullptr) return false;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) continue;
      if (op->kind == OpKind::Break) {
        if (i + 1 != r->size()) {
          if (getenv("LN_DBG2")) fprintf(stderr, "[LN2] realBreak top-level i=%zu size=%zu\n", i, r->size());
          return true;
        }
        continue;
      }
      // ★ **内层循环**（`While`/`For`）里的 `BreakOp` 是**内层**的 `break` 或
      //   `continue` ⇒ 本层循环一律不规范化。
//   …（原有 6 行说明，已并入 S06 报告）
      //   ⚠️ 本分支必须**先于** `if (op->kind != OpKind::If) continue;`。
      if (op->kind == OpKind::While || op->kind == OpKind::For) {
        if (regionHasBreak(op->region(1))) return true;
        continue;
      }
      if (op->kind != OpKind::If) continue;
      // ★ **这个 `If` 之后还有实质语句吗？**（与 `checkContinues` **同一判据**，
      //   必须一致：两处若不一致，一个放行、一个拦下，行为就取决于调用顺序了）
      //   没有 ⇒ 分支末尾的 `Break` 是真 `break`（见上面的长注释）。
      const bool hasAfter = hasRealStmtAfter(r, i);
      for (size_t k = 0; k < op->numRegions(); ++k) {
        const Region* sub = op->region(k);
        if (sub == nullptr || sub->empty()) continue;
        const bool brk = sub->back() != nullptr && sub->back()->kind == OpKind::Break;
        if (brk && hasAfter) continue;      // 跳过了 after 语句 ⇒ `continue`
        if (brk && !hasAfter) {
          if (getenv("LN_DBG2")) {
            fprintf(stderr, "[LN2] realBreak If@%u (no after stmt)\n",
                    (unsigned)op->loc.line);
          }
          return true;
        }
        if (hasRealBreak(sub)) {
          if (getenv("LN_DBG2")) fprintf(stderr, "[LN2] realBreak in If@%u sub\n", (unsigned)op->loc.line);
          return true;
        }
      }
    }
    return false;
  }

  // 【后置】子树里有没有 `ContinueOp`（**任意深度**）。与 `regionHasBreak` 同形。
  static bool regionHasContinue(const Region* r) {
    if (r == nullptr) return false;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) continue;
      if (op->kind == OpKind::Continue) return true;
      for (size_t k = 0; k < op->numRegions(); ++k) {
        if (regionHasContinue(op->region(k))) return true;
      }
    }
    return false;
  }

  // 【后置】子树里有没有 `BreakOp`（**任意深度**，含内层循环体）。
  static bool regionHasBreak(const Region* r) {
    if (r == nullptr) return false;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) continue;
      if (op->kind == OpKind::Break) return true;
      for (size_t k = 0; k < op->numRegions(); ++k) {
        if (regionHasBreak(op->region(k))) return true;
      }
    }
    return false;
  }

  // 【后置】这个 Region 是否以**内层循环**（`While`/`For`）收尾。
  //   用途：`while (i<n) { if (…) { i=i+1; continue; } j=0; while (j<n) {…} i=i+1; }`
  //   里 `If.else` 只含 `(Yield)`、而 `if` **之后**的 `While_j` 是内层循环 ——
  //   它的自增属于内层循环，不能算进外层的路径计数
  //   （第一版没挡这一条，于是 `01_mm1.sy` 的外层循环被判"步进 ≥2"）。
  static bool endsWithNestedLoop(const Region* r) {
    if (r == nullptr || r->empty()) return false;
    const Op* last = r->back();
    if (last == nullptr || last->kind == OpKind::Yield || last->kind == OpKind::Break ||
        last->kind == OpKind::Continue) {
      // 终结符在最后 ⇒ 看倒数第二个（IRGen 的形状）
      if (r->size() < 2) return false;
      last = r->at(r->size() - 2);
    }
    return last != nullptr && (last->kind == OpKind::While || last->kind == OpKind::For);
  }

  // 直接路径（"正常走完一轮"）上的自增数（-1 = 形状不合法）。
  //   `IfOp`：某分支以 `Break` 收尾 ⇒ 那是 `continue` 分支，**不计入直接路径**；
  //   两个分支都不以 `Break` 收尾 ⇒ 两个分支的计数**必须相等**（体的其余部分
  //   在两条路径上一样多）。
  int directPathIncrements(const Region* r) const {
    if (r == nullptr) return -1;
    int n = 0;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr || op->kind == OpKind::Break || op->kind == OpKind::Continue) continue;
      if (isIvIncrement(op, ivSlot_, nullptr)) { ++n; continue; }
      if (op->kind != OpKind::If) continue;
      const Region* a = op->region(0);
      const Region* b = op->region(1);
      if (a == nullptr || b == nullptr || a->empty() || b->empty()) return -1;
      const bool ab = a->back() != nullptr && a->back()->kind == OpKind::Continue;
      const bool bb = b->back() != nullptr && b->back()->kind == OpKind::Continue;
      if (ab && bb) return -1;
      if (ab || bb) continue;
      // ⚠️ 分支整体是一个**内层循环**（`While`/`For`）⇒ 不递归：那个循环体的
      //   自增属于内层循环（本 pass 处理内层时会各自判定）。
      const int ca = endsWithNestedLoop(a) ? 0 : directPathIncrements(a);
      const int cb = endsWithNestedLoop(b) ? 0 : directPathIncrements(b);
      if (ca < 0 || cb < 0 || ca != cb) return -1;
      n += ca;
    }
    return n;
  }

  // 每个"分支末尾 continue"的分支必须恰好 1 个自增（`continue` 也要走到下一轮
  //   ⇒ 那条路径也必须 +1）。顶层尾部 `Break` 的自增也检查（`i=i+1; continue;`）。
  bool continueBranchesHaveOne(const Region* r, int* count) const {
    if (r == nullptr) return true;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) continue;
      if (op->kind == OpKind::Break) return false;   // 顶层 Break 已被 checkContinues 拒绝
      if (op->kind != OpKind::If) continue;
      for (size_t k = 0; k < op->numRegions(); ++k) {
        const Region* sub = op->region(k);
        if (sub == nullptr || sub->empty()) continue;
        if (sub->back() != nullptr && sub->back()->kind == OpKind::Continue) {
          int c = 0;
          for (size_t q = 0; q < sub->size(); ++q) {
            if (isIvIncrement(sub->at(q), ivSlot_, nullptr)) ++c;
          }
          if (c != 1) return false;
          *count += c;
          continue;
        }
        // ⚠️ **不递归进内层循环的体**：内层的 `BreakOp`/自增属于内层循环
        //    （本 pass 会在处理内层循环时各自判定）。
        if (sub->back() != nullptr && isControlFlowContainer(sub->back()->kind)) continue;
        if (!continueBranchesHaveOne(sub, count)) return false;
      }
    }
    return true;
  }

  // ── ⑤ 改写（只在真跑调用）──────────────────────────────────────────────
  //   摘掉体内**全部** IV 自增（`ForOp` 的步进对**每条路径**生效），并把
  //   `continue` 降级出来的 `BreakOp` 换成 `(Yield)`。
  void applyStep() {
    resolveContinues();
    stripAllIvIncrements(body_);
  }

  // 递归：把"体里最后一个语句是 `IfOp` 时，其分支末尾的 `BreakOp`"换成
  //   `(Yield)`（那是 `continue`）。其余位置的 `BreakOp` 不进这里
  //   （`hasRealBreak` 已经把真 `break` 挡在门外，体尾的尾部 `continue` 由
  //   `applyStep` 末尾单独处理）。
  size_t stripAllIvIncrements(Region* r) {
    if (r == nullptr) return 0;
    size_t n = 0;
    std::vector<Op*> v = r->ops();
    std::vector<Op*> kept;
    kept.reserve(v.size());
    bool changed = false;
    for (Op* op : v) {
      if (op == nullptr) { kept.push_back(op); continue; }
      if (isIvIncrement(op, ivSlot_, nullptr)) { ++n; changed = true; continue; }
      if (op->kind == OpKind::If) {
        for (size_t k = 0; k < op->numRegions(); ++k) n += stripAllIvIncrements(op->region(k));
      }
      kept.push_back(op);
    }
    if (changed) r->replaceAll(kept);
    return n;
  }

  // ── 小工具 ─────────────────────────────────────────────────────────────
  void give(SkipReason r) {
    if (!failed_) { failed_ = true; fail_ = r; }
  }
  bool failed() const { return failed_; }
  SkipReason failReason() const { return fail_; }
  // 【后置】边界表达式里有没有 `Load <本地 Alloca 槽>`。
  //   用途：体内含 `CallOp` 时，只有"边界读本地槽"才构成"槽可能被写"的风险。
  bool boundReadsLocalSlot() const {
    if (upper_ == nullptr) return true;   // 保守
    std::unordered_set<const Op*> seen;
    std::vector<const Op*> st{upper_->definer};
    while (!st.empty()) {
      const Op* op = st.back();
      st.pop_back();
      if (op == nullptr || !seen.insert(op).second) continue;
      if (op->kind == OpKind::Load && op->numOperands() == 1) {
        const Op* slot = (op->operand(0) != nullptr) ? op->operand(0)->definer : nullptr;
        if (slot != nullptr && slot->kind == OpKind::Alloca) return true;
      }
      for (size_t i = 0; i < op->numOperands(); ++i) {
        const Value v = op->operand(i);
        st.push_back(v != nullptr ? v->definer : nullptr);
      }
    }
    return false;
  }
  Value ivSlot() const { return ivSlot_; }
  Value upper() const { return upper_; }
  const Op* ivLoad() const { return ivLoad_; }
  size_t continuesResolved() const { return continuesResolved_; }
  size_t tailContinues() const { return tailContinues_; }

  Op* mk(OpKind k, SourceLoc loc) { return arena_.makeOp(k, loc); }
  Region* mkRegion() { return arena_.makeRegion(); }

 private:
  Arena& arena_;
  Op* loop_ = nullptr;
  Region* body_ = nullptr;
  std::vector<Value> openGuards_;
  Value ivSlot_ = nullptr;
  Value upper_ = nullptr;
  const Op* ivLoad_ = nullptr;
  bool failed_ = false;
  SkipReason fail_ = SkipReason::CondNotLt;
  size_t continuesResolved_ = 0;   // 被包装的 continue 数（`if (!cond) { B }`）
  size_t tailContinues_ = 0;       // 其中"换成 (Yield)"的分支/尾部 continue 数
};


// ============================================================================
// 一次判定（**只读**：不改树）
// ============================================================================
bool analyzeOnce(Op* loop, Analyzer& a) {
  Region* condR = loop->region(0);
  Region* bodyR = loop->region(1);
  if (condR == nullptr || bodyR == nullptr) { a.give(SkipReason::CondNotLt); return false; }
  if (!a.checkContinues(bodyR)) a.give(SkipReason::HasBreak);   // ① 只读判定
  if (!a.failed()) a.findIv(condR);          // ②
  if (!a.failed()) a.checkBound();           // ③
  if (!a.failed()) {                         // 体内有 Call ⇒ 边界的循环不变性还证得出来吗
    bool call = false;
    walkOps(bodyR, [&](const Op* op) { if (op->kind == OpKind::Call) call = true; });
    if (call && a.boundReadsLocalSlot()) a.give(SkipReason::CallInBody);
  }
  if (!a.failed()) a.analyzeStep();          // ④
  return !a.failed();
}

}  // namespace

// ============================================================================
// 对外的唯一入口
// ============================================================================
RewriteResult analyzeAndRewriteLoop(Op* loop, Arena& arena) {
  RewriteResult r;
  Analyzer a(arena, loop, loop->region(1));
  if (!analyzeOnce(loop, a)) {
    r.fail = a.failReason();
    return r;
  }
  a.applyStep();
  r.ok = true;
  r.iv = a.ivSlot();
  r.upper = a.upper();
  r.ivLoad = a.ivLoad();
  r.continuesResolved = a.continuesResolved();
  r.tailContinues = a.tailContinues();
  return r;
}

}  // namespace detail
}  // namespace sir
}  // namespace sysy
