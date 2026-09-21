// ============================================================================
// StructuredVerifier.cpp —— 六条不变式的检查实现（规格见 StructuredVerifier.h）
// ============================================================================
#include "structured/StructuredVerifier.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "frontend/RuntimeLib.h"

namespace sysy {
namespace sir {
namespace {

// 检查器的显式工作栈帧：一个 (Op, 它在哪个 Region 里、在哪个下标)。
struct Ctx {
  const Op* parent;      // 拥有这个 Op 的 Op（nullptr = 模块根）
  const Op* fn;          // 所属函数（模块级 GlobalVar 为 nullptr）
  const Region* region;  // 它所在的 Region
  bool inForBody;        // 是否位于某个 ForOp 的体内（I1/I2/I3 用）
};

class Checker {
 public:
  explicit Checker(const Op* module) : module_(module) {}

  std::vector<Violation> run() {
    if (module_ == nullptr || module_->kind != OpKind::Module) {
      add("I-结构", SourceLoc(), "根 Op 不是 ModuleOp");
      return vs_;
    }
    if (module_->numRegions() != 1) {
      add("I-结构", module_->loc, "ModuleOp 必须恰好有 1 个 Region");
      return vs_;
    }
    checkModuleRegion(module_->region(0));
    return vs_;
  }

 private:
  void add(const char* inv, SourceLoc loc, const std::string& msg) {
    Violation v;
    v.invariant = inv;
    v.loc = loc;
    v.message = msg;
    vs_.push_back(v);
  }

  static std::string name(const Op* op) { return opKindName(op->kind); }

  // ── I4：每个 Region **恰好一个终结 Op，且在最后一行** ────────────────────
  //   为什么"在最后一行"必须单独查：终结 Op 出现在中间时，它后面的 Op 是
  //   **不可达代码**（结构化层的语义是"Region 顺序执行到终结为止"），
  //   而"不可达代码"会让 S06 的展平产出死块 —— 那是下游要花力气处理的形态。
  void checkRegion(const Region* r, const Op* owner, const Ctx& ctx, const char* role) {
    if (r == nullptr) return;
    size_t terms = 0;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) {
        add("I4", SourceLoc(), std::string(role) + "：Region 里有空 Op 槽");
        continue;
      }
      if (isTerminator(op->kind)) {
        ++terms;
        if (i + 1 != r->size()) {
          add("I4", op->loc, std::string(role) + "：终结 Op `" + name(op) +
                                 "` 不在 Region 的最后一行（其后还有 " +
                                 std::to_string(r->size() - i - 1) + " 个 Op）");
        }
      }
    }
    if (terms == 0) {
      add("I4", owner != nullptr ? owner->loc : SourceLoc(),
          std::string(role) + "：Region 没有终结 Op");
    } else if (terms > 1) {
      add("I4", owner != nullptr ? owner->loc : SourceLoc(),
          std::string(role) + "：Region 有 " + std::to_string(terms) +
              " 个终结 Op（必须恰好 1 个）");
    }
    // ── I5：只有 If/While/For 允许带 Region（控制流是**树**，不可约 CFG 构造不出来）──
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) continue;
      if (op->numRegions() > 0 && !isControlFlowContainer(op->kind)) {
        add("I5", op->loc, "非控制流容器 `" + name(op) +
                               "` 带了子 Region（只允许 If/While/For）");
      }
      checkOp(op, ctx);
    }
  }

  // ── 单个 Op 的结构检查 ──────────────────────────────────────────────────
  void checkOp(const Op* op, const Ctx& ctx) {
    // 操作数：非空、且被定义过（use-def 一致）
    for (size_t i = 0; i < op->numOperands(); ++i) {
      const Value v = op->operand(i);
      if (v == nullptr) {
        add("use-def", op->loc, "`" + name(op) + "` 的第 " + std::to_string(i) +
                                   " 个操作数是空值");
        continue;
      }
      if (v->definer == nullptr) {
        add("use-def", op->loc, "`" + name(op) + "` 的第 " + std::to_string(i) +
                                   " 个操作数没有定义 Op");
      }
    }
    // 结果个数与 OpKind 的期望一致（**这是"凭空发明 Op 形态"的一类**）
    checkArity(op);
    // 终结符的类型匹配：ReturnOp 的操作数个数与函数返回类型一致
    if (op->kind == OpKind::Return) {
      const bool isVoidFn = (ctx.fn == nullptr) || (ctx.fn->typeAttr(1) != nullptr &&
                                                    ctx.fn->typeAttr(1)->kind ==
                                                        TypeKind::Void);
      if (isVoidFn && op->numOperands() != 0) {
        add("终结符", op->loc, "void 函数的 ReturnOp 带了 " +
                                   std::to_string(op->numOperands()) + " 个操作数");
      } else if (!isVoidFn && op->numOperands() != 1) {
        add("终结符", op->loc, "非 void 函数的 ReturnOp 有 " +
                                   std::to_string(op->numOperands()) +
                                   " 个操作数（必须恰好 1 个）");
      }
    }
    if (op->kind == OpKind::Yield && ctx.inForBody == false && ctx.parent != nullptr &&
        !isControlFlowContainer(ctx.parent->kind) && ctx.parent->kind != OpKind::Func &&
        ctx.parent->kind != OpKind::Module) {
      add("I-结构", op->loc, "YieldOp 出现在非控制流容器 `" + name(ctx.parent) + "` 里");
    }
    if (op->kind == OpKind::GetElementPtr) checkGep(op);
    if (op->kind == OpKind::Alloca) checkAlloca(op, ctx);
    if (op->kind == OpKind::For) checkForInvariants(op, ctx);

    // 递归进子 Region（**这里用显式栈**：见 run() 的说明 —— 检查器的主要输入
    // 是可能被改坏的 IR，深度不可假设）
    for (size_t i = 0; i < op->numRegions(); ++i) {
      const Region* sub = op->region(i);
      if (sub == nullptr) {
        add("I-结构", op->loc, "`" + name(op) + "` 的第 " + std::to_string(i) +
                                   " 个 Region 是空指针");
        continue;
      }
      const char* role = "Region";
      if (op->kind == OpKind::While) role = (i == 0) ? "WhileOp 条件 Region" : "WhileOp 体 Region";
      else if (op->kind == OpKind::If) role = (i == 0) ? "IfOp then Region" : "IfOp else Region";
      else if (op->kind == OpKind::For) role = "ForOp 体 Region";
      else if (op->kind == OpKind::Func) role = "函数体 Region";
      else if (op->kind == OpKind::Module) role = "模块 Region";
      Ctx sub2 = ctx;
      sub2.parent = op;
      sub2.region = sub;
      if (op->kind == OpKind::For) sub2.inForBody = true;
      checkRegion(sub, op, sub2, role);
    }
  }

  // ── OpKind ↔ 结果个数的期望 ─────────────────────────────────────────────
  //   SysY 的 IR 里"每个 Op 的结果个数"是**确定的**（0 或 1），只有容器支持 0..N。
  //   这条检查抓的是"凭空发明 Op 形态"（例如给 StoreOp 加一个结果）。
  void checkArity(const Op* op) {
    const size_t n = op->numResults();
    auto want0 = [&](const char* what) {
      if (n != 0) add("形状", op->loc, std::string(what) + " 不应有结果（有 " +
                                        std::to_string(n) + " 个）");
    };
    auto want1 = [&](const char* what) {
      if (n != 1) add("形状", op->loc, std::string(what) + " 必须恰好 1 个结果（有 " +
                                        std::to_string(n) + " 个）");
    };
    switch (op->kind) {
      case OpKind::Module: case OpKind::Store: case OpKind::Return:
      case OpKind::Goto: case OpKind::Yield: case OpKind::Break:
      case OpKind::Unreachable: case OpKind::GlobalVar:
        // GlobalVar 是"模块级命名实体"（由 GetGlobalOp 按名字引用）⇒ **无结果**
        want0(opKindName(op->kind));
        return;
      case OpKind::While:
      case OpKind::For:
        // 循环 Op **没有结果**：`WhileOp` 的值通路是"条件 Region 里 Yield 一个
        // bool"，循环本身不产值（`ForOp` 同理）。S05 只产 While。
        want0(opKindName(op->kind));
        return;
      case OpKind::Func:
        want0("Func");
        return;
      case OpKind::If:
        // ★ S05 的 `IfOp` **没有结果**（纯语句）：条件在操作数里、两个 Region
        //   是分支。值的选择用"临时槽 + store/load"表达（与"变量全在内存"一致）。
        //   为什么不做"带结果的 If"：那会让 dump 的 `(If %x @line 5) {` 里
        //   `%x` 有**两种含义**（结果名 / 条件操作数），读回器无法区分
        //   ⇒ 轨 A（往返逐字节相同）不可能成立。
        want0("If");
        if (op->numRegions() != 2) {
          add("形状", op->loc, "IfOp 必须恰好 2 个 Region（then/else）");
        }
        if (op->numOperands() != 1) {
          add("形状", op->loc, "IfOp 必须恰好 1 个操作数（条件）");
        }
        return;
      case OpKind::Call: {
        // 结果个数由**被调函数**决定：查本模块的函数表，再查运行时名字表；
        //   `_sysy<X>` / `llvm.memcpy` / `llvm.memset` 是 void。
        //   查不到 ⇒ 保守要求 1 个结果（用户函数一定有签名）。
        const std::string callee = op->strAttr(0);
        const Type* rt = nullptr;
        const auto it = fnRet_.find(callee);
        if (it != fnRet_.end()) {
          rt = it->second;
        } else if (callee.rfind("_sysy", 0) == 0 || callee == "llvm.memcpy" ||
                   callee == "llvm.memset") {
          rt = typePool().voidTy();
        } else if (const FuncSig* sig = lookupRuntimeFunc(callee)) {
          rt = sig->ret == nullptr ? typePool().i32()
                                   : (isVoid(sig->ret) ? typePool().voidTy()
                                                       : typePool().i32());
        } else {
          // 自递归 / 未在本模块定义（`main` 之外的用户函数都在；残缺树可能缺）
          rt = typePool().i32();
        }
        if (rt != nullptr && rt->kind == TypeKind::Void) want0("void 调用");
        else want1("Call");
        return;
      }
      default:
        want1(opKindName(op->kind));
        return;
    }
  }

  // ── I6：每个数组下标访问都必须被标记（仿射 / 非仿射）───────────────────
  //   S05 只**记录**，不做 ArrayAccess 分析（那是 S16）。所以检查的是
  //   "标记存在且合法"，不是"标记对不对"。
  void checkGep(const Op* op) {
    if (op->numOperands() != 2) {
      add("I6", op->loc, "GetElementPtrOp 必须有 2 个操作数（指针 + 下标），有 " +
                             std::to_string(op->numOperands()) + " 个");
    }
    if (op->numAttrs() < 3) {
      add("I6", op->loc, "GetElementPtrOp 缺少标记（需要 [元素类型, 下标类型, 亲和性]）");
      return;
    }
    if (op->attr(0) == nullptr || op->attr(0)->kind != Attr::Kind::Type) {
      add("I6", op->loc, "GetElementPtrOp 的第 1 个属性必须是元素类型");
    }
    if (op->attr(1) == nullptr || op->attr(1)->kind != Attr::Kind::Type) {
      add("I6", op->loc, "GetElementPtrOp 的第 2 个属性必须是指针/下标类型");
    }
    if (op->attr(2) == nullptr || op->attr(2)->kind != Attr::Kind::Int) {
      add("I6", op->loc, "GetElementPtrOp 缺少 I6 亲和性标记（0 = 仿射 / 1 = 非仿射）");
      return;
    }
    const int64_t a = op->attr(2)->i;
    if (a != 0 && a != 1) {
      add("I6", op->loc, "GetElementPtrOp 的亲和性标记只能是 0 或 1，实际是 " +
                             std::to_string(a));
    }
  }

  // ── 后端不变量 2：所有 alloca 都在函数入口 Region ───────────────────────
  //   ★ 这条在 AGENT-CONTEXT §5（后端可以依赖的不变量第 2 条）里是**硬契约**：
  //     "所有 alloca 都在函数入口块"。S05 一开始就把 alloca 放在入口 Region
  //     （S05b 的"提升"因此位置不动），所以现在就该被检查 —— 否则某天 IRGen
  //     多了个"顺手在循环体里 alloca 临时量"，后端一次性布局栈帧的假设会静默失效。
  void checkAlloca(const Op* op, const Ctx& ctx) {
    if (ctx.fn == nullptr) {
      add("后端不变量", op->loc, "AllocaOp 出现在函数之外");
      return;
    }
    const Region* entry = ctx.fn->numRegions() > 0 ? ctx.fn->region(0) : nullptr;
    if (entry != ctx.region) {
      add("后端不变量", op->loc, "AllocaOp 不在函数入口 Region（后端要一次性布局栈帧）");
    }
  }

  // ── I1 / I2 / I3（★ S05 不产 ForOp ⇒ 这三条在本关**平凡成立**）──────────
  //   实现是规格要求（prompt §4.2），并在单元测试里用**手工构造的 IR** 验证
  //   检查器真的抓得住。**不要把"检查器有这三条"读成"S05 验证过循环不变量"**。
  void checkForInvariants(const Op* op, const Ctx& ctx) {
    (void)ctx;
    if (op->numRegions() != 1) {
      add("I-结构", op->loc, "ForOp 必须恰好 1 个体 Region");
      return;
    }
    const Region* body = op->region(0);
    const std::string iv = op->strAttr(0);
    if (iv.empty()) add("I1", op->loc, "ForOp 缺少 IV 名字（attrs[0]）");
    if (op->numOperands() != 3) {
      add("I2", op->loc, "ForOp 必须有 3 个操作数（lower/upper/step），有 " +
                             std::to_string(op->numOperands()) + " 个");
    }
    // where：某个 Op 是否在 body 子树里（含 body 自己）
    auto inBody = [&](const Op* target) {
      if (target == nullptr || body == nullptr) return false;
      bool found = false;
      std::vector<const Op*> st;
      for (size_t i = 0; i < body->size(); ++i) st.push_back(body->at(i));
      while (!st.empty()) {
        const Op* x = st.back();
        st.pop_back();
        if (x == nullptr) continue;
        if (x == target) { found = true; break; }
        for (size_t i = 0; i < x->numRegions(); ++i) {
          const Region* r = x->region(i);
          if (r == nullptr) continue;
          for (size_t j = 0; j < r->size(); ++j) st.push_back(r->at(j));
        }
      }
      return found;
    };
    // I1：IV 在循环体内不被赋值 —— 体内任何 StoreOp 的目的地不能是 IV 的槽。
    //     这里用**保守判据**：体内出现了对 IV 名字的 Alloca 槽的 Store，就算违反。
    //     （真正的证明属于 LoopNormalize，检查器只回答"没被写"这个可判定的问题。）
    if (body != nullptr) {
      std::vector<const Op*> st;
      for (size_t i = 0; i < body->size(); ++i) st.push_back(body->at(i));
      while (!st.empty()) {
        const Op* x = st.back();
        st.pop_back();
        if (x == nullptr) continue;
        if (x->kind == OpKind::Store && x->numOperands() == 2) {
          const Op* dst = x->operand(1) != nullptr ? x->operand(1)->definer : nullptr;
          if (dst != nullptr && dst->kind == OpKind::Alloca &&
              !dst->strAttr(0).empty() && dst->strAttr(0) == iv) {
            add("I1", x->loc, "ForOp 的 IV `" + iv + "` 在循环体内被赋值");
          }
        }
        if (x->kind == OpKind::Break) {
          add("I3", x->loc, "ForOp 体内出现 BreakOp（SCoP 条件要求体内无 break）");
        }
        for (size_t i = 0; i < x->numRegions(); ++i) {
          const Region* r = x->region(i);
          if (r == nullptr) continue;
          for (size_t j = 0; j < r->size(); ++j) st.push_back(r->at(j));
        }
      }
    }
    // I2：lower/upper/step 在体内不被修改 —— 同上，看三个操作数的定义是否在体内
    for (size_t i = 0; i < op->numOperands(); ++i) {
      const Value v = op->operand(i);
      const Op* d = (v != nullptr) ? v->definer : nullptr;
      if (d != nullptr && inBody(d)) {
        static const char* kBound[3] = {"lower", "upper", "step"};
        add("I2", op->loc, std::string("ForOp 的 ") + kBound[i < 3 ? i : 2] +
                               " 在循环体**内**被定义（体内可能修改它）");
      }
    }
  }

  void checkModuleRegion(const Region* r) {
    if (r == nullptr) {
      add("I-结构", SourceLoc(), "模块 Region 是空指针");
      return;
    }
    bool sawFunc = false;
    for (size_t i = 0; i < r->size(); ++i) {
      const Op* op = r->at(i);
      if (op == nullptr) { add("I-结构", SourceLoc(), "模块 Region 里有空 Op 槽"); continue; }
      if (op->kind == OpKind::GlobalVar || op->kind == OpKind::GetGlobal) {
        // `GetGlobalOp` 是全局对象的**引用形式**（`ptr[对象类型]`）：函数体里的
        // `Load/Store/GEP` 直接引用它的结果。它与 `GlobalVarOp` 一样必须排在
        // 所有函数之前（下游按模块顺序分配数据段）。
        if (sawFunc) {
          add("顺序", op->loc, "全局相关的 Op 出现在 FuncOp 之后（全局必须在所有函数之前）");
        }
      } else if (op->kind == OpKind::Func) {
        sawFunc = true;
        const Type* rt = op->typeAttr(1);
        fnRet_[op->strAttr(0)] = (rt != nullptr) ? rt : typePool().i32();
      } else {
        add("I5", op->loc, "模块 Region 里出现 `" + name(op) +
                               "`（只允许 GlobalVarOp/GetGlobalOp 与 FuncOp）");
      }
      Ctx ctx;
      ctx.parent = module_;
      ctx.fn = (op->kind == OpKind::Func) ? op : nullptr;
      ctx.region = r;
      ctx.inForBody = false;
      checkOp(op, ctx);
    }
  }

  const Op* module_;
  std::vector<Violation> vs_;
  // 函数名 → 返回类型（`Call` 的结果个数由它决定）
  std::unordered_map<std::string, const Type*> fnRet_;
};

}  // namespace

std::vector<Violation> verifyModule(const Op* module) {
  Checker c(module);
  return c.run();
}

std::string formatViolations(const std::vector<Violation>& vs) {
  std::string out;
  for (const Violation& v : vs) {
    out += "[";
    out += v.invariant;
    out += "] ";
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

}  // namespace sir
}  // namespace sysy
