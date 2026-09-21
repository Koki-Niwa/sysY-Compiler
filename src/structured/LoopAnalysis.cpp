// ============================================================================
// LoopAnalysis.cpp —— 判定的只读实现（规格见 LoopAnalysis.h）
// ============================================================================
#include "structured/LoopAnalysis.h"

namespace sysy {
namespace sir {

ParentMap buildParents(const Op* root) {
  ParentMap par;
  if (root == nullptr) return par;
  std::vector<const Op*> work{root};
  while (!work.empty()) {
    const Op* op = work.back();
    work.pop_back();
    if (op == nullptr) continue;
    for (size_t i = 0; i < op->numRegions(); ++i) {
      const Region* r = op->region(i);
      if (r == nullptr) continue;
      for (size_t j = 0; j < r->size(); ++j) {
        const Op* sub = r->at(j);
        if (sub == nullptr) continue;
        par[sub] = std::make_pair(op, r);
        work.push_back(sub);
      }
    }
  }
  return par;
}

bool isInBody(const ParentMap& par, const Region* body, const Op* op) {
  for (const Op* x = op; x != nullptr;) {
    const auto it = par.find(x);
    if (it == par.end()) return false;
    if (it->second.second == body) return true;
    x = it->second.first;
  }
  return false;
}

bool isLoadOf(const Value v, const Value slot) {
  const Op* d = (v != nullptr) ? v->definer : nullptr;
  return d != nullptr && d->kind == OpKind::Load && d->numOperands() == 1 &&
         d->operand(0) == slot;
}

bool isOne(const Value v) {
  const Op* d = (v != nullptr) ? v->definer : nullptr;
  if (d == nullptr || d->kind != OpKind::Int) return false;
  const Attr* a = d->attr(0);
  return a != nullptr && a->kind == Attr::Kind::Int && a->i == 1;
}

bool isIvIncrement(const Op* op, const Value slot, const Op** addOut) {
  if (op == nullptr || op->kind != OpKind::Store || op->numOperands() != 2) return false;
  if (op->operand(1) != slot) return false;
  const Op* add = (op->operand(0) != nullptr) ? op->operand(0)->definer : nullptr;
  if (add == nullptr || add->kind != OpKind::AddI || add->numOperands() != 2) return false;
  const bool l = isLoadOf(add->operand(0), slot);
  const bool r = isLoadOf(add->operand(1), slot);
  const bool lo = isOne(add->operand(1));
  const bool ro = isOne(add->operand(0));
  if (!((l && lo) || (r && ro))) return false;
  if (addOut != nullptr) *addOut = add;
  return true;
}

const Op* rootSlot(const Value p) {
  const Op* d = (p != nullptr) ? p->definer : nullptr;
  size_t guard = 0;
  while (d != nullptr && d->kind == OpKind::GetElementPtr && guard++ < 64) {
    d = (d->operand(0) != nullptr) ? d->operand(0)->definer : nullptr;
  }
  return (d != nullptr && d->kind == OpKind::Alloca) ? d : nullptr;
}

std::unordered_set<const Op*> writtenSlotsIn(const Region* body) {
  std::unordered_set<const Op*> out;
  walkOps(body, [&](const Op* op) {
    if (op->kind != OpKind::Store || op->numOperands() != 2) return;
    const Op* s = rootSlot(op->operand(1));
    if (s != nullptr) out.insert(s);
  });
  return out;
}

bool provePure(const Value root, const Value ivSlot,
               const std::unordered_set<const Op*>& written) {
  std::unordered_set<const Op*> seen;
  std::vector<const Op*> st{root != nullptr ? root->definer : nullptr};
  while (!st.empty()) {
    const Op* op = st.back();
    st.pop_back();
    if (op == nullptr || !seen.insert(op).second) continue;
    switch (op->kind) {
      case OpKind::Int:
      case OpKind::Float:
      case OpKind::AddI: case OpKind::SubI: case OpKind::MulI:
      case OpKind::DivI: case OpKind::ModI: case OpKind::MinusI:
      case OpKind::AddF: case OpKind::SubF: case OpKind::MulF:
      case OpKind::DivF: case OpKind::MinusF:
      case OpKind::Eq: case OpKind::Ne: case OpKind::Lt:
      case OpKind::Le: case OpKind::Gt: case OpKind::Ge:
      case OpKind::I2F: case OpKind::F2I: case OpKind::Sext:
      case OpKind::Select: case OpKind::Bitcast:
      case OpKind::GetElementPtr:
      // ★ 两条**纯定义**也必须在允许之列：`bound = Load(ptr=…)` 的指针来源
      //   就是它们。漏掉任何一条都会让"边界是环境/全局"的循环全部判成非仿射：
      //     * `Alloca`   —— 局部标量槽（第一版漏了它，三个变体一个都没规范化）；
      //     * `GetGlobal`—— 全局对象的引用形式（第二个变体 `sl1.sy` 的
      //       `while (i < N - 1)` 就撞在这里：`N` 是 `const int N = 4096`，
      //       读它要经过 `GetGlobal`）。
      case OpKind::Alloca:
      case OpKind::GetGlobal:
        break;
      case OpKind::Load: {
        const Op* slot = (op->operand(0) != nullptr) ? op->operand(0)->definer : nullptr;
        if (slot != nullptr && slot == ivSlot->definer) return false;   // 依赖 IV ⇒ 非仿射
        if (slot != nullptr && written.count(slot) != 0) return false;  // 体内被写
        break;
      }
      default:
#ifdef LN_DEBUG_PURE
        fprintf(stderr, "[LN] provePure reject kind=%s line=%u\n", opKindName(op->kind),
                (unsigned)op->loc.line);
#endif
        return false;   // Call/Store/控制流容器/未知形态
    }
    for (size_t i = 0; i < op->numOperands(); ++i) {
      const Value v = op->operand(i);
      st.push_back(v != nullptr ? v->definer : nullptr);
    }
  }
  return true;
}

std::vector<Op*> condCandidates(const Region* condR) {
  std::vector<Op*> out;
  if (condR == nullptr) return out;
  for (size_t i = 0; i < condR->size(); ++i) {
    Op* op = condR->at(i);
    if (op != nullptr && !isTerminator(op->kind)) out.push_back(op);
  }
  return out;
}

std::string dumpResultName(const Op* fn, const Op* target) {
  if (fn == nullptr || target == nullptr) return std::string();
  const std::string fname = fn->strAttr(0);
  uint32_t counter = 0;
  std::vector<const Op*> st;
  for (size_t i = 0; i < fn->numRegions(); ++i) {
    const Region* r = fn->region(i);
    if (r == nullptr) continue;
    for (size_t j = r->size(); j-- > 0;) st.push_back(r->at(j));
  }
  while (!st.empty()) {
    const Op* op = st.back();
    st.pop_back();
    if (op == nullptr) continue;
    for (size_t k = 0; k < op->numResults(); ++k) {
      if (op == target && k == 0) return "%" + fname + "." + std::to_string(counter);
      ++counter;
    }
    for (size_t i = op->numRegions(); i-- > 0;) {
      const Region* r = op->region(i);
      if (r == nullptr) continue;
      for (size_t j = r->size(); j-- > 0;) st.push_back(r->at(j));
    }
  }
  return std::string();
}

}  // namespace sir
}  // namespace sysy
