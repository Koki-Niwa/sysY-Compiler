// ============================================================================
// ir/FlatVerifier.cpp —— 平面 IR 的不变量检查器（规格见 FlatVerifier.h）
//
//   本文件是**只读**的：它不改 IR 一个字节（所以可以在任何 pass 之后调用）。
//   唯一的"写"是构造支配树时的内部状态。
// ============================================================================
#include "ir/FlatVerifier.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace sysy {
namespace flat {
namespace {

// 35 个**真** opcode 的容器内表示（`Nop` 与两个常量不是指令；
// `LLVMMemCpy`/`LLVMMemSet` 属于 `call` 家族）。
//   ⚠️ 这里只是**编译期自查**；判据的权威是 `check_flat.py` 里那份
//   **独立手写**的清单（从 C++ 枚举生成等于自己证明自己，prompt §三.2）。
bool isRealOpcode(Opcode k) {
  switch (k) {
    case Opcode::Nop:
    case Opcode::ConstantInt:
    case Opcode::ConstantFP:
    case Opcode::kCount:
      return false;
    default:
      return true;
  }
}

bool isCallFamily(Opcode k) {
  return k == Opcode::Call || k == Opcode::LLVMMemCpy || k == Opcode::LLVMMemSet;
}

std::string kindName(Opcode k) {
  if (k == Opcode::ConstantInt || k == Opcode::ConstantFP) return "常量";
  return opcodeName(k);
}

}  // namespace

// ============================================================================
// 支配树（Cooper-Harvey-Kennedy）
// ============================================================================
DominatorTree::DominatorTree(const Function& f) {
  const size_t n = f.blockCount();
  reach_.assign(n, 0);
  post_.assign(n, -1);
  idom_.assign(n, nullptr);
  if (n == 0) return;

  // ① 从入口块出发做**显式栈** DFS，得到后序编号
  std::vector<int> order;            // 后序序列（块下标）
  order.reserve(n);
  std::vector<std::pair<BasicBlock*, size_t>> st;   // (块, 下一个后继下标)
  std::vector<char> seen(n, 0);
  st.emplace_back(f.entry(), 0);
  seen[0] = 1;
  while (!st.empty()) {
    std::pair<BasicBlock*, size_t>& top = st.back();
    BasicBlock* b = top.first;
    Instruction* t = b->terminator();
    const size_t nsucc = (t != nullptr) ? t->numSuccs() : 0;
    bool advanced = false;
    while (top.second < nsucc) {
      BasicBlock* s = t->succ(top.second++);
      if (s == nullptr || s->parent() != &f) continue;
      const size_t si = s->index();
      if (si >= n || seen[si]) continue;
      seen[si] = 1;
      st.emplace_back(s, 0);
      advanced = true;
      break;
    }
    if (advanced) continue;
    order.push_back(static_cast<int>(b->index()));
    st.pop_back();
  }
  for (size_t i = 0; i < order.size(); ++i) {
    post_[static_cast<size_t>(order[i])] = static_cast<int>(i);
  }
  for (size_t i = 0; i < n; ++i) reach_[i] = seen[i];

  // ② 逆后序（RPO）：后序的逆序
  std::vector<BasicBlock*> rpo;
  rpo.reserve(order.size());
  for (size_t i = order.size(); i-- > 0;) rpo.push_back(f.at(static_cast<size_t>(order[i])));

  // ③ 迭代求 idom 到不动点
  auto intersect = [&](BasicBlock* a, BasicBlock* b) -> BasicBlock* {
    while (a != b) {
      while (post_[a->index()] < post_[b->index()]) a = idom_[a->index()];
      while (post_[b->index()] < post_[a->index()]) b = idom_[b->index()];
    }
    return a;
  };
  idom_[f.entry()->index()] = f.entry();   // 哨兵：入口的 idom 是它自己
  bool changed = true;
  while (changed) {
    changed = false;
    for (BasicBlock* b : rpo) {
      if (b == f.entry()) continue;
      BasicBlock* newIdom = nullptr;
      for (size_t i = 0; i < b->numPreds(); ++i) {
        BasicBlock* p = b->pred(i);
        if (p == nullptr || p->parent() != &f) continue;
        if (idom_[p->index()] == nullptr) continue;       // 还没算出来
        newIdom = (newIdom == nullptr) ? p : intersect(p, newIdom);
      }
      if (newIdom != nullptr && idom_[b->index()] != newIdom) {
        idom_[b->index()] = newIdom;
        changed = true;
      }
    }
  }
  // 入口块的 idom 对外是 nullptr（"没有支配者"）
  idom_[f.entry()->index()] = nullptr;
}

bool DominatorTree::dominates(const BasicBlock* a, const BasicBlock* b) const {
  if (a == nullptr || b == nullptr) return false;
  if (a == b) return true;
  const size_t bi = b->index();
  if (bi >= reach_.size() || !reach_[bi]) return false;
  const BasicBlock* cur = b;
  // 沿 idom 链上溯（**迭代**；链长 ≤ 块数）
  size_t guard = 0;
  while (cur != nullptr && guard++ <= reach_.size()) {
    if (cur == a) return true;
    const size_t ci = cur->index();
    if (ci >= idom_.size()) return false;
    cur = idom_[ci];
  }
  return false;
}

// ============================================================================
// 检查
// ============================================================================
std::vector<Violation> verifyFlatModule(const Module& m) {
  std::vector<Violation> bad;
  auto add = [&](const char* inv, const std::string& fn, SourceLoc loc,
                 const std::string& msg) {
    bad.push_back(Violation{inv, loc, msg, fn});
  };

  for (size_t fi = 0; fi < m.numFunctions(); ++fi) {
    Function* f = m.function(fi);
    if (f == nullptr || f->isDeclaration()) continue;
    const std::string& fname = f->name();
    // ★ S06 排障设施：`SYSY_DBG_CFG=1` 打印**校验器看到的边**。
    //   为什么需要：φ 的入值块集合与前驱集合不符时，光看文本 dump 分不清是
    //   "边没建出来"还是"φ 的键错了" —— 这两个方向的修法完全相反。
    if (const char* dbg = std::getenv("SYSY_DBG_CFG"); dbg != nullptr && *dbg != '0') {
      for (BasicBlock* b : f->blocks()) {
        Instruction* t = b->terminator();
        std::fprintf(stderr, "[CFG] %s L%u 终=%s 后继=%u", fname.c_str(),
                     static_cast<unsigned>(b->index()),
                     t != nullptr ? opcodeName(t->op()) : "(无)",
                     t != nullptr ? static_cast<unsigned>(t->numSuccs()) : 0u);
        if (t != nullptr) {
          for (size_t k = 0; k < t->numSuccs(); ++k) {
            std::fprintf(stderr, " L%d", t->succ(k) != nullptr
                                            ? static_cast<int>(t->succ(k)->index())
                                            : -1);
          }
        }
        std::fprintf(stderr, " | 前驱=%u", static_cast<unsigned>(b->numPreds()));
        for (size_t k = 0; k < b->numPreds(); ++k) {
          std::fprintf(stderr, " L%d", b->pred(k) != nullptr
                                           ? static_cast<int>(b->pred(k)->index())
                                           : -1);
        }
        std::fprintf(stderr, "\n");
      }
    }

    // ── V6：指令集封闭 + 结果类型一致性 ────────────────────────────────
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      for (size_t ii = 0; ii < b->size(); ++ii) {
        Instruction* in = b->at(ii);
        if (!isRealOpcode(in->op())) {
          add("V6", fname, in->loc(),
              "出现了非指令的 opcode：" + kindName(in->op()) + "（容器占位不该被发射）");
          continue;
        }
        // 结果类型：固定类型必须与 `fixedResultType` 一致
        const Type* want = fixedResultType(in->op());
        if (want != nullptr && in->type() != want) {
          add("V6", fname, in->loc(),
              std::string("结果类型与 opcode 不符：") + opcodeName(in->op()) + " 应为 " +
                  typeText(want) + "，实际 " + typeText(in->type()));
        }
        // 操作数个数
        const size_t nops = in->numOperands();
        const bool okArity =
            (in->isBinOp() && nops == 2) ||
            (in->op() == Opcode::FNeg && nops == 1) ||
            (in->op() == Opcode::Load && nops == 1) ||
            (in->op() == Opcode::Store && nops == 2) ||
            (in->op() == Opcode::GEP && nops == 2) ||
            (in->op() == Opcode::BitCast && nops == 1) ||
            (in->op() == Opcode::ICmp && nops == 2) ||
            (in->op() == Opcode::FCmp && nops == 2) ||
            (in->op() == Opcode::Select && nops == 3) ||
            (in->op() == Opcode::Alloca && nops == 0) ||
            (in->op() == Opcode::Phi && nops >= 1) ||
            (isCallFamily(in->op())) ||
            (in->op() == Opcode::SIToFP && nops == 1) ||
            (in->op() == Opcode::FPToSI && nops == 1) ||
            (in->op() == Opcode::FPExt && nops == 1) ||
            (in->op() == Opcode::SExt && nops == 1) ||
            (in->op() == Opcode::ZExt && nops == 1) ||
            (in->op() == Opcode::Trunc && nops == 1) ||
            (in->op() == Opcode::Ret) ||
            (in->op() == Opcode::Br) ||
            (in->op() == Opcode::Unreachable);
        if (!okArity) {
          add("V6", fname, in->loc(),
              std::string("操作数个数不合规：") + opcodeName(in->op()) + " 有 " +
                  std::to_string(nops) + " 个");
        }
        // 内存指令：**只查"操作数是不是指针 / 值类型是否为空"**。
        //   ⚠️ 这里**不查** "`load <ty>` 的 `ty` 必须等于指针的元素类型"。
        //   原因（一个用实测换来的结论）：结构化层的类型推导有两处已知污染
        //   （`StructuredReader` 在**解析属性之前**就算 `GetElementPtr` 的结果
        //   类型 ⇒ 多包一层 `ptr`，而那个类型又被别处的推导链引用），
        //   而那份 dump 是**冻结契约**（往返必须逐字节相同）⇒ 不能改读回器。
        //   平面层于是按"结构化层的 `<ty>` 优先"来展开，两种口径在少数
        //   **数组形参/数组对象**的 load 上不一致。判据的**权威**因此落在
        //   另一条上：`check_flat.py` 轨 B 用独立实现从文本重判类型一致性，
        //   而"结果类型与指针元素不一致"这一条在那边按**同一份 dump 的口径**
        //   检查（不会把结构性差异当成红）。
        if (in->op() == Opcode::Load && nops == 1) {
          Value* p = in->operand(0);
          const Type* pt = p != nullptr ? p->type() : nullptr;
          if (pt == nullptr || !pt->isPtr()) {
            add("V6", fname, in->loc(), "load 的操作数不是指针类型");
          }
        }
        if (in->op() == Opcode::Store && nops == 2) {
          Value* v = in->operand(0);
          Value* p = in->operand(1);
          const Type* pt = p != nullptr ? p->type() : nullptr;
          if (pt == nullptr || !pt->isPtr()) {
            add("V6", fname, in->loc(), "store 的目标不是指针类型");
          }
          if (v != nullptr && v->type() == nullptr) {
            add("V6", fname, in->loc(), "store 的值没有类型");
          }
        }
        // ⚠️ GEP **不要求**"基址的元素类型 == `<ty>`"：结构化层的
        //   `GetElementPtr <ty> <idxty> 0 ptr idx` 是**带类型的偏移运算**
        //   （`<ty>` 说明"结果指向什么"，可以比基址的元素更靠里，例如
        //    数组形参 `int arr[][2]` 的 `arr[i][j]`：基址 `ptr[ptr[[2 x i32]]]`、
        //    `<ty>` 是 `i32`）。后端按"结果类型 = ptr[<ty>]"发射即可。
        if (in->op() == Opcode::GEP && nops == 2) {
          const Type* et = in->srcElemType();
          Value* p = in->operand(0);
          Value* idx = in->operand(1);
          if (et == nullptr || p == nullptr || idx == nullptr) {
            add("V6", fname, in->loc(), "getelementptr 缺少元素类型或操作数");
          } else if (in->type() != typePool().ptrTo(et)) {
            add("V6", fname, in->loc(), "getelementptr 的结果类型应为 ptr[" +
                                            typeText(et) + "]，实际 " + typeText(in->type()));
          } else if (p->type() == nullptr || !p->type()->isPtr()) {
            add("V6", fname, in->loc(), "getelementptr 的基址不是指针");
          } else if (idx->type() != typePool().i64()) {
            add("V6", fname, in->loc(), "getelementptr 的下标必须是 i64（i32 里算完再 sext）");
          }
        }
        // 终结符不能出现在中间（V4 的另一半）
        if (isTerminatorOpcode(in->op()) && ii + 1 != b->size()) {
          add("V4", fname, in->loc(),
              std::string("终结符不是最后一条：") + opcodeName(in->op()) + " 后面还有指令");
        }
      }
    }

    // ── V4：每个块恰好一个终结符，且在最后 ────────────────────────────
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      Instruction* t = b->terminator();
      if (t == nullptr) {
        add("V4", fname, SourceLoc(),
            "块 L" + std::to_string(bi) + " 没有终结符（每个块恰好一个）");
        continue;
      }
      size_t nterm = 0;
      for (size_t ii = 0; ii < b->size(); ++ii) {
        if (isTerminatorOpcode(b->at(ii)->op())) ++nterm;
      }
      if (nterm != 1) {
        add("V4", fname, t->loc(),
            "块 L" + std::to_string(bi) + " 有 " + std::to_string(nterm) + " 个终结符");
      }
      // `br` 的目标必须属于同一个函数
      for (size_t k = 0; k < t->numSuccs(); ++k) {
        BasicBlock* s = t->succ(k);
        if (s == nullptr) {
          add("V4", fname, t->loc(), "分支目标为空");
        } else if (s->parent() != f) {
          add("V4", fname, t->loc(), "分支目标不属于同一个函数");
        }
      }
      if (t->op() == Opcode::Br && t->numSuccs() != 1 && t->numSuccs() != 2) {
        add("V4", fname, t->loc(), "br 的后继个数只能是 1 或 2");
      }
      if (t->op() == Opcode::Br && t->numSuccs() == 2 && t->numOperands() != 1) {
        add("V4", fname, t->loc(), "条件 br 必须恰好一个条件操作数");
      }
      if (t->op() == Opcode::Ret) {
        const bool voidFn = f->retType() == nullptr || f->retType()->isVoid();
        if (!voidFn && t->numOperands() != 1) {
          add("V4", fname, t->loc(), "非 void 函数的 ret 必须带一个返回值");
        } else if (!voidFn && t->operand(0) != nullptr && t->operand(0)->type() != f->retType()) {
          add("V4", fname, t->loc(),
              "ret 的值类型与函数返回类型不符：" + typeText(t->operand(0)->type()) + " vs " +
                  typeText(f->retType()));
        } else if (voidFn && t->numOperands() != 0) {
          add("V4", fname, t->loc(), "void 函数的 ret 不能带返回值");
        }
      }
    }

    // ── V5：`alloca` 全在入口块（不变量 2）────────────────────────────
    BasicBlock* entry = f->entry();
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      if (b == entry) continue;
      for (size_t ii = 0; ii < b->size(); ++ii) {
        if (b->at(ii)->op() == Opcode::Alloca) {
          add("V5", fname, b->at(ii)->loc(),
              "alloca 出现在非入口块 L" + std::to_string(bi) + "（不变量 2）");
        }
      }
    }
    // ⚠️ V5 **只查"全在入口块"**，不查"是否连续排在块首"。
    //   为什么：`alloca` 的**初始化顺序**是语义的一部分
    //   （`int x = 0; int y = x + 1;` 的 store 顺序不能动），
    //   而结构化层的每个 `Alloca` 后面紧跟它自己的初始化 Store。
    //   把 alloca 全部挪到块首会**改变 store 的相对顺序** ⇒ 语义风险。
    //   后端要的是"能一次性扫出入口块的全部 alloca 来布局栈帧"，
    //   这与"非 alloca 指令排在它们之间"并不冲突（扫一遍即可）。
    //   ⇒ 判据 = "非入口块里 alloca 数 == 0"（上面那个循环），没有第二条。

    // ── V1：use-def 双向一致 ──────────────────────────────────────────
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      for (size_t ii = 0; ii < b->size(); ++ii) {
        Instruction* in = b->at(ii);
        for (size_t k = 0; k < in->numOperands(); ++k) {
          Value* v = in->operand(k);
          if (v == nullptr) {
            add("V1", fname, in->loc(),
                std::string("操作数为空：") + opcodeName(in->op()));
            continue;
          }
          // 模块级 interned 常量（`ValueKind::Constant`）**没有 parent 且合法**
          //   —— `Module::getIntConst` 的产物就是这种形态。第一版只排除了
          //   "指令形态的常量"，于是手工构造的 CFG（用 `getIntConst`）被误报
          //   "操作数指向一条不属于任何块的指令"（`test_flat.cpp` 的菱形断言
          //   抓出来的）。
          // 模块级 interned 常量没有 parent 且合法
          if (v->isInternedConstant()) continue;
          if (v->isInst()) {
            Instruction* d = static_cast<Instruction*>(v);
            if (d->isParam()) continue;               // 形参：入口块开头，不是指令
            if (d->isConstant()) continue;
            if (d->parent() == nullptr) {
              add("V1", fname, in->loc(), "操作数指向一条不属于任何块的指令");
            }
            // 反向边必须存在（**两个方向都查**）
            const std::vector<Instruction*>& us = d->users();
            if (std::find(us.begin(), us.end(), in) == us.end()) {
              add("V1", fname, in->loc(),
                  "反向 use-def 缺失：使用者没有登记在定义者的 users() 里");
            }
          } else if (!v->isInternedConstant() && !v->isGlobalAddr() &&
                     !v->isBlockAddr()) {
            add("V1", fname, in->loc(), "操作数不是可用的值形态");
          }
        }
      }
    }
    // 反向：每个登记的 user 必须真的用到我
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      for (size_t ii = 0; ii < b->size(); ++ii) {
        Instruction* d = b->at(ii);
        for (Instruction* u : d->users()) {
          bool uses = false;
          if (u != nullptr) {
            for (size_t k = 0; k < u->numOperands(); ++k) {
              if (u->operand(k) == static_cast<Value*>(d)) { uses = true; break; }
            }
          }
          if (!uses) {
            add("V1", fname, d->loc(),
                "反向 use-def 过期：users() 里有一条并不使用它的指令");
          }
        }
      }
    }
    // 参数侧的 use-def
    for (size_t pi = 0; pi < f->numParams(); ++pi) {
      Instruction* p = f->param(pi);
      if (p == nullptr) {
        add("V1", fname, SourceLoc(), "形参表里有空项");
        continue;
      }
      for (Instruction* u : p->users()) {
        bool uses = false;
        for (size_t k = 0; u != nullptr && k < u->numOperands(); ++k) {
          if (u->operand(k) == static_cast<Value*>(p)) { uses = true; break; }
        }
        if (!uses) {
          add("V1", fname, SourceLoc(), "形参的 users() 里有并不使用它的指令");
        }
      }
    }

    // ── V2：SSA 支配（**真的算支配树**）────────────────────────────────
    DominatorTree dt(*f);
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      if (!dt.reachable(bi)) continue;     // 不可达块：不谈支配（单独由 V4 管）
      for (size_t ii = 0; ii < b->size(); ++ii) {
        Instruction* in = b->at(ii);
        // φ 的入值单独按"支配对应前驱块"检查（见 V3）
        if (in->op() == Opcode::Phi) continue;
        for (size_t k = 0; k < in->numOperands(); ++k) {
          Value* v = in->operand(k);
          if (v == nullptr || !v->isInst()) continue;
          Instruction* d = static_cast<Instruction*>(v);
          const BasicBlock* db = d->isParam() ? f->entry() : d->parent();
          if (db == nullptr) continue;      // V1 已经报过
          if (!dt.dominates(db, b)) {
            add("V2", fname, in->loc(),
                "使用点不被定义支配：值定义在 L" + std::to_string(db->index()) +
                    "，使用在 L" + std::to_string(bi));
          }
        }
      }
    }

    // ── V3：φ 的合法性 ────────────────────────────────────────────────
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* b = f->at(bi);
      // ① φ 只在块首、连续排列
      bool phiEnded = false;
      for (size_t ii = 0; ii < b->size(); ++ii) {
        Instruction* in = b->at(ii);
        if (in->op() == Opcode::Phi) {
          if (phiEnded) {
            add("V3", fname, in->loc(),
                "φ 不在块首连续排列（块 L" + std::to_string(bi) + "）");
          }
          continue;
        }
        phiEnded = true;
      }
      for (size_t ii = 0; ii < b->size(); ++ii) {
        Instruction* phi = b->at(ii);
        if (phi->op() != Opcode::Phi) continue;
        // ② 入值个数 == 前驱个数（**入口块没有前驱：它的 φ 是非法的**）
        if (b == f->entry()) {
          add("V3", fname, phi->loc(), "入口块不该有 φ（入口块没有前驱）");
          continue;
        }
        if (phi->numOperands() != b->numPreds()) {
          add("V3", fname, phi->loc(),
              "φ 的入值个数（" + std::to_string(phi->numOperands()) +
                  "）与前驱个数（" + std::to_string(b->numPreds()) + "）不符");
        }
        // ③ 前驱集合 == 入值块集合（**完全相同**，不是"包含"）
        std::unordered_set<const BasicBlock*> phiPreds, realPreds;
        for (size_t k = 0; k < phi->numSuccs(); ++k) {
          if (phi->succ(k) != nullptr) phiPreds.insert(phi->succ(k));
        }
        for (size_t k = 0; k < b->numPreds(); ++k) realPreds.insert(b->pred(k));
        if (phiPreds != realPreds) {
          std::string d = "φ 的前驱 {";
          for (const BasicBlock* pb : phiPreds) d += "L" + std::to_string(pb->index()) + " ";
          d += "} vs 块前驱 {";
          for (const BasicBlock* pb : realPreds) d += "L" + std::to_string(pb->index()) + " ";
          d += "}";
          add("V3", fname, phi->loc(), "φ 的入值块集合与块的前驱集合不同：" + d);
        }
        // ④ 每个入值支配对应的前驱块；且**至少一个非自身入值**
        size_t selfRefs = 0;
        for (size_t k = 0; k < phi->numOperands(); ++k) {
          Value* v = phi->operand(k);
          BasicBlock* pb = phi->succ(k);
          if (v == static_cast<const Value*>(phi)) ++selfRefs;
          if (v == nullptr || pb == nullptr) continue;
          if (v->isInst()) {
            Instruction* d = static_cast<Instruction*>(v);
            const BasicBlock* db = d->isParam() ? f->entry() : d->parent();
            if (db != nullptr && !dt.dominates(db, pb)) {
              add("V3", fname, phi->loc(),
                  "φ 的入值不支配对应的前驱块 L" + std::to_string(pb->index()));
            }
          }
        }
        if (selfRefs == phi->numOperands()) {
          add("V3", fname, phi->loc(), "φ 的入值全部指向它自己（至少一个非自身入值）");
        }
      }
    }
  }
  return bad;
}

std::string formatViolations(const std::vector<Violation>& vs) {
  std::string out;
  for (const Violation& v : vs) {
    out += "  [";
    out += v.invariant;
    out += "] ";
    if (!v.func.empty()) {
      out += v.func;
      out += ": ";
    }
    if (v.loc.line != 0) {
      out += "line ";
      out += std::to_string(v.loc.line);
      out += ": ";
    }
    out += v.message;
    out += '\n';
  }
  return out;
}

}  // namespace flat
}  // namespace sysy
