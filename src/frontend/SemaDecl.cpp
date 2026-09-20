// ============================================================================
// SemaDecl.cpp —— 声明、维度、初始化器（S03）
//
// 规范依据：`docs/sysy_lang.txt` §3 ConstDef / §3 VarDef / §3 Initial Values /
// §3 FuncFParam。
//
// ============================================================================
// ★★ 初始化器的形状检查：C 语义的"扁平游标 + 花括号层级"
// ============================================================================
//   规范把三种**非法**形式点名列出（§3 Initial Values 2）：
//       a[4] = 4            （数组用裸标量初始化）
//       a[2] = {{1,2}, 3}   （花括号嵌套比秩更深）
//       a = {1,2,3}         （标量用花括号组初始化）
//   又给出三种**合法**的嵌套写法（§3 ConstDef 6，以 a[3][2] 为例）：
//       {{1,2},{3,4},{5,6}} / {1,2,3,4,5,6} / {1,2,{3,4},5,6}
//   以及 §3 ConstDef 7 的 `int d[4][2] = {1, 2, {3}, {5}, 7, 8};`
//   （扁平标量与嵌套组**混用**）。
//
//   判据（prompt §3.2 第 15 条，四条规定）：
//     ① 花括号组的嵌套深度不得深于目标的**秩**；
//     ② 一个组在某一维上消耗掉的元素个数不得超过该维长度；
//     ③ 元素总数不得超过数组元素总数（§3 ConstDef 10）；
//     ④ 整型数组的元素不许是浮点（§3 ConstDef 8 的**不对称**规则；
//        反过来"浮点数组用整型初值"是合法的）。
//
//   ★ 实现模型（与 C 一致，且能同时解释上面全部例子）：
//     * 维护一个**扁平游标** `initPos_`（行主序的下一个待填元素下标）；
//     * 一个**花括号组**对应一段"子对象区间" `[start, start + elemSize(level))`；
//       组结束时游标**跳到区间末端**（所以 `{{1},{3,4},5,6}` 里 `{1}` 会把
//       整个 a[0] 填满并把游标推到 a[1]）；
//     * 组内的**标量**元素只把游标 +1（所以 `{1,2,3,4,5,6}` 能扁平填满
//       `int[3][2]`）；
//     * 组内的**嵌套组**层级恒为 level+1；当 `level+1 > 秩-1` 时就是
//       "嵌套比秩更深" ⇒ 报 E-INIT-SHAPE。
//     这套模型对 §3 ConstDef 7 的 6 个例子逐条给出规范里写明的结果
//     （`{{1,2},{3,0},{5,0}}` / `{{0,0},{3,4},{5,6}}` / …）。
//
//   ⚠️ 遍历必须**迭代**：`{{{{…}}}}`（prompt §九点名）在递归实现下会栈溢出，
//      所以每个组是一个 `TK::InitGroup` 帧，深度不设人为上限。
//
// ============================================================================
// ★ 常量求值的时机与"单一算法"原则
// ============================================================================
//   全局变量与 const 对象的初始化器必须是**常量表达式**（规范 §3 Initial
//   Values 1 / §3 ConstDef 5），而 const 对象的**元素值**还要能被后续维度
//   引用（实测 `63_big_int_mul.sy` 的 `int c1[len + 5];` 引用全局常量）。
//   ⇒ 形状检查与常量求值走**同一趟**遍历：形状游标顺便就是常量值的下标。
//   （若另写一个"求值专用"的走法，两份形状算法必然漂移 —— 那正是这类项目
//     最常见的隐性缺陷。）
//   求值与形状是**串行**的：一个 VarDef 的初始化器任务全部跑完，才轮到下一个
//   VarDef，所以下面的 scratch 状态不需要做成显式栈。
// ============================================================================
#include <string>
#include <utility>
#include <vector>

#include "frontend/Sema.h"

namespace sysy {

namespace D = sema_diag;

namespace {
// 目标数组从第 level 维起的元素个数（行主序子对象大小）。维度未知时返回 -1。
int64_t elemSizeAt(const Type* arr, int level) { return elementCount(dropDims(arr, level)); }
}  // namespace

// ============================================================================
// 一、Decl：逐个 VarDef（**同一个 Decl 内**的重名也在 VarDef 里判）
// ============================================================================
void Sema::doDecl(Frame& f) {
  auto* d = static_cast<Decl*>(f.node);
  if (d == nullptr || f.idx >= d->defs.size()) return;
  VarDef* v = d->defs[f.idx].get();
  Frame next = f;
  next.idx = f.idx + 1;
  push(next);
  if (v != nullptr) pushVarDef(d, v);
}

// ============================================================================
// 二、VarDef：求维度 → 建类型 → 声明符号 → 检查初始化器
// ============================================================================
void Sema::doVarDef(Frame& f) {
  auto* v = static_cast<VarDef*>(f.node);
  const Decl* d = f.decl;
  if (v == nullptr || d == nullptr) return;

  const bool isGlobal = scopes_.atGlobalScope();
  const Type* ty = buildArrayType(d->base, v->dims, /*isArrayParam=*/false);
  v->semType = ty;

  // ② 声明符号。
  //    * 同作用域重复定义 ⇒ E-REDEF（规范 §3 Conventions：同名局部变量的
  //      作用域不得重叠；顶层 §3.2 CompUnit 2 连"类型不同"都不许）
  //    * **块内遮蔽外层是合法的**（规范 §3 Block 2），所以只查当前作用域
  //    * 变量名**可以**与函数名相同（规范 §3 Conventions 3）；运行时库函数名
  //      也照此办理 —— 变量遮蔽它之后，`putch(...)` 会走 E-CALL-NONFUNC。
  //      （规范只禁止"用户**函数**与运行时库重名"，那条在 doFuncDef 里。）
  Symbol* slot = nullptr;
  if (scopes_.declaredInCurrentScope(v->name)) {
    error(v->loc, D::kRedef, (isGlobal ? "顶层重复定义 '" : "同作用域重复定义 '") +
                                 v->name + "'");
  } else {
    Symbol s;
    s.kind = d->isConst ? SymKind::Const : SymKind::Var;
    s.type = ty;
    s.isConst = d->isConst;
    s.loc = v->loc;
    slot = scopes_.declare(v->name, s);
  }

  // 维度表达式本身也是表达式：Sema 必须访问它们，转储里才不会有 `:?`
  // （`(Dim (IntLit 2 :int))`，见 prompt §五 的例子）。
  // 执行顺序：dims → 初始化器 → VarDefFinal ⇒ 按逆序压栈。
  Frame fin;
  fin.kind = TK::VarDefFinal;
  push(fin);
  for (size_t i = v->dims.size(); i-- > 0;) {
    Dim& dm = v->dims[i];
    if (dm.expr != nullptr) pushExpr(*dm.expr, &dm.expr, nullptr, 0);
  }

  if (v->init == nullptr) return;   // 没有初始化器：全局由 S04 零初始化，局部值未定义

  // ③ 初始化器：形状检查 + 元素类型检查 + 插转换（任务机）
  //    常量性：**全局变量**（规范 §3 Initial Values 1）与**所有 const 对象**
  //    （规范 §3 ConstDef 5：ConstInitVal 里的表达式是 ConstExp）都必须常量。
  const bool needConst = isGlobal || d->isConst;
  // ★ "检查常量性"与"把值留下来"是**两件事**，必须分开：
  //   * needConst     —— 要求初始化器是常量表达式（全局：规范 §3 Initial Values 1；
  //                      const 对象：规范 §3 ConstDef 5）。**非 const 全局也要查。**
  //   * materialize   —— 把逐元素的值存进 consts_，供**后续 ConstExp 引用**
  //                      （规范 §3 ConstDef 3："may refer to already-defined symbolic constants"）。
  //                      只有 **const 对象**才可能被引用到。
  //   ⚠️ 曾经这两件事共用一个条件（`isGlobal || d->isConst`），于是
  //      `int buffer[50000000] = {};`（23_json.sy:355）按每元素 12 字节物化了 **574 MB**，
  //      而那份数据**一个字节都不可能被读到**：实测 `int g[2]={1,2}; int a[g[0]];`
  //      报 E-ARRAY-DIM（非 const 对象不是符号常量），`int N=3; int M=N;` 报 E-CONST-INIT。
  //      开销随源码里的一个数字线性无界 ⇒ 合法程序 `int a[200000000] = {};` 要 ~2.4 GB。
  const bool materialize = d->isConst;
  initNeedConst_ = needConst;
  initMaterialize_ = materialize;
  initConstOk_ = true;
  initType_ = ty;
  initSym_ = slot;
  //   ★ S04：占位表示是 ConstObject（默认值 + 稀疏非零表），所以这里的"分配"
  //     是 **O(1)** 的。**不许**把它退回成"按元素 assign"：
  //     `const int a[50000000] = {1};` 是合法程序，按元素物化要 **600 MB**
  //     （prompt §一 的第 3 条硬约束；同一条约束的另一半见 ConstEval.h 的
  //      ConstObject 注释与 InitPlan.h 的 GlobalData）。
  //     非 const 对象照样写这份占位（写入路径带边界检查），但 `initMaterialize_`
  //     是 false ⇒ doVarDefFinal 不会把它挂到符号上。
  initScratch_ = ConstObject{};
  initScratch_.type = ty;
  initScratch_.dflt = ConstValue::ofInt(0);
  initScratch_.scalar = ConstValue::ofInt(0);

  pushInitRoot(v->init.get(), ty, needConst);  // 在 VarDefFinal 之前执行
}

// 初始化器处理完毕后：把求出的常量挂到符号上（供后续维度/常量表达式引用）。
void Sema::doVarDefFinal(Frame&) {
  if (initMaterialize_ && initConstOk_ && initSym_ != nullptr) {
    // 标量对象：值存在 `scalar` 里（数组才看 `nonzero` / `dflt`）。
    if (initType_ != nullptr && !initType_->isArray()) initScratch_.scalar = initScratch_.dflt;
    consts_.push_back(std::move(initScratch_));
    initSym_->cval = &consts_.back();
  }
  initScratch_ = ConstObject{};
  initNeedConst_ = false;
  initMaterialize_ = false;
  initSym_ = nullptr;
}

// ============================================================================
// 三、初始化器入口
// ============================================================================
void Sema::pushInitRoot(InitVal* iv, const Type* target, bool needConst) {
  Frame f;
  f.kind = TK::InitRoot;
  f.init = iv;
  f.target = target;
  f.flags = needConst ? kInitNeedConst : 0;
  push(f);
}

void Sema::pushInitGroup(InitVal* iv, const Type* target, int level, int depth, int64_t start,
                         uint8_t flags) {
  Frame f;
  f.kind = TK::InitGroup;
  f.init = iv;
  f.target = target;
  f.level = level;
  f.depth = depth;
  f.start = start;
  f.idx = 0;
  f.flags = flags;
  push(f);
}

void Sema::doInitRoot(Frame& f) {
  InitVal* iv = f.init;
  const Type* target = f.target;
  if (iv == nullptr || target == nullptr) return;
  const uint8_t nf = static_cast<uint8_t>(f.flags & kInitNeedConst);
  const uint8_t ef = static_cast<uint8_t>(nf | kInitArrElem);

  if (rank(target) == 0) {
    // ── 标量目标 ────────────────────────────────────────────────────────
    if (iv->expr != nullptr) {
      if ((nf & kInitNeedConst) != 0) evalConstInto(*iv->expr, 0, target);
      // 标量初始化允许 float→int（规范 §3 Implicit Type Conversions 的例子
      // 就是 `int i = 4.0;`）⇒ **不带** kInitArrElem
      pushExpr(*iv->expr, &iv->expr, target, 0);
      return;
    }
    // `= {}` / `= {1}` / `= {1,2,3}`：花括号组用于标量 ⇒ 嵌套深度 1 > 秩 0
    error(iv->loc, D::kInitShape,
          "标量对象不能用花括号组初始化（规范 §3 Initial Values 2：`a = {1,2,3}` 非法）");
    // 仍然遍历组内的表达式，保证转储类型完整
    for (size_t i = iv->list.size(); i-- > 0;) {
      InitVal* ch = iv->list[i].get();
      if (ch != nullptr && ch->expr != nullptr) {
        pushExpr(*ch->expr, &ch->expr, target, 0);
      }
    }
    return;
  }

  // ── 数组目标 ──────────────────────────────────────────────────────────
  if (iv->expr != nullptr) {
    // `a[4] = 4`：数组不能用裸标量初始化（规范 §3 Initial Values 2）
    error(iv->loc, D::kInitShape,
          "数组不能用裸标量初始化（规范 §3 Initial Values 2：`a[4] = 4` 非法）");
    pushExpr(*iv->expr, &iv->expr, nullptr, 0);
    return;
  }
  initPos_ = 0;
  initTotal_ = elementCount(target);
  if (initTotal_ < 0) initTotal_ = 0;
  pushInitGroup(iv, target, /*level=*/0, /*depth=*/1, /*start=*/0, ef);
}

// ============================================================================
// 四、一个花括号组
// ============================================================================
void Sema::doInitGroup(Frame& f) {
  InitVal* g = f.init;
  const Type* target = f.target;
  if (g == nullptr || target == nullptr) return;

  const int r = rank(target);
  const Type* et = elementType(target);
  // 本组覆盖的子对象区间 [start, start + elemSize(level))。
  // `level == r` 表示"花括号作用在一个**标量**位置上"（C 的花括号省略语义），
  // 此时区间长度是 1。
  const int64_t cap = (f.level >= r) ? 1 : elemSizeAt(target, f.level);
  const int64_t end = f.start + (cap < 0 ? 0 : cap);

  if (f.idx >= g->list.size()) {
    // 本组结束：元素过多 ⇒ 超过该维长度 / 数组元素总数（规范 §3 ConstDef 10）
    if (initPos_ > end || initPos_ > initTotal_) {
      error(g->loc, D::kInitShape,
            "初始化器的元素个数超过了该维长度（规范 §3 ConstDef 6/10）");
    }
    initPos_ = end;   // ★ 组结束 ⇒ 游标跳到子对象区间末端（C 语义：
                      //   `{{1},{3,4},5,6}` 里 `{1}` 会把整个 a[0] 填满）
    return;
  }

  InitVal* child = g->list[f.idx].get();
  Frame next = f;
  next.idx = f.idx + 1;
  push(next);
  if (child == nullptr) return;

  if (child->expr != nullptr) {
    // ── 标量元素：占一个扁平位置 ────────────────────────────────────────
    if (initPos_ >= end || initPos_ >= initTotal_) {
      error(child->loc, D::kInitShape,
            "初始化器的元素个数超过了数组元素总数（规范 §3 ConstDef 10）");
      pushExpr(*child->expr, &child->expr, nullptr, 0);   // 仍然分析，转储才完整
      return;
    }
    if ((f.flags & kInitNeedConst) != 0) evalConstInto(*child->expr, initPos_, et);
    initPos_ += 1;
    pushExpr(*child->expr, &child->expr, et, static_cast<uint8_t>(kInitArrElem));
    return;
  }

  // ── 嵌套花括号组 ──────────────────────────────────────────────────────
  //  ① 深度判据：花括号的**嵌套层数**不得超过目标的秩（prompt §3.2 第 15 条 ①）
  const int nd = f.depth + 1;
  if (nd > r) {
    error(child->loc, D::kInitShape,
          "花括号组的嵌套深度（" + std::to_string(nd) + "）超过了目标的秩（" +
              std::to_string(r) + "）（规范 §3 Initial Values 2：`a[2] = {{1,2},3}` 非法）");
    for (size_t i = child->list.size(); i-- > 0;) {
      InitVal* gch = child->list[i].get();
      if (gch != nullptr && gch->expr != nullptr) {
        pushExpr(*gch->expr, &gch->expr, nullptr, 0);
      }
    }
    return;
  }
  //  ② 这一组作用在**哪一层子对象**上：从 level+1 起找**最浅**的、当前游标
  //     正好对齐（`pos % elemSize(k) == 0`）的维度层。
  //     这就是 C 的"当前对象"：`{1,2,3,4,{5}}` 对 `int[2][3][4]` 时游标在 4，
  //     4 对 elemSize(2)=4 对齐、对 elemSize(1)=12 不对齐 ⇒ `{5}` 初始化的是
  //     i[0][1]（第 2 层），而不是 i[0]（第 1 层）。实测语料
  //     语料 `h_functional/07_arr_init_nd.sy:8` 的
  //     `int i[2][3][4] = {1, 2, 3, 4, {5}, {}};` 正是这一形态。
  //     找不到任何对齐的聚合层 ⇒ 花括号作用在标量位置上（level = r）。
  int lvl = r;
  for (int k = f.level + 1; k <= r - 1; ++k) {
    const int64_t sz = elemSizeAt(target, k);
    if (sz > 0 && initPos_ % sz == 0) { lvl = k; break; }
  }
  pushInitGroup(child, target, lvl, nd, initPos_, static_cast<uint8_t>(f.flags & kInitNeedConst));
}

// ============================================================================
// 五、常量求值（把值写进 initScratch_[pos]）
// ============================================================================
void Sema::evalConstInto(Expr& e, int64_t pos, const Type* elemType) {
  ConstValue v;
  if (!eval_.eval(e, v, D::kConstInit)) {
    initConstOk_ = false;
    return;
  }
  // 按目标元素类型归一化（`float b[2] = {1,2}` 合法：int 初值转 float；
  // 反过来在整型数组里已经报过 E-TYPE，这里只是不让它污染常量值）
  if (elemType != nullptr) {
    if (isInt(elemType) && v.isFloat) {
      v = ConstValue::ofInt(satFptosi(v.f));
    } else if (isFloat(elemType) && !v.isFloat) {
      v = ConstValue::ofFloat(static_cast<float>(v.i));
    }
  }
  if (pos >= 0) {
    const int64_t n = initScratch_.count();
    if (n > 0 && pos < n) {
      const uint64_t idx = static_cast<uint64_t>(pos);
      if (initScratch_.isScalar()) {
        initScratch_.dflt = v;          // 标量：唯一那个元素就是"默认值"槽
        initScratch_.scalar = v;
        initScratch_.nonzero.clear();
      } else {
        initScratch_.setElem(idx, v);
      }
    }
  }
}

// ============================================================================
// 六、类型构造（逐维求值）
//
//   规范 §3 ConstDef 3/4：每一维都必须显式给出长度，且"能在编译期求出
//   **非负整数**"。§3 FuncFParam 2：形参第一维省略（写成 `[]`），
//   **其余每一维必须是整型常量**。
//   ★ 语法上形参的非首维写的是 `Exp` 而不是 `ConstExp`（规范 §3 文法），
//     所以"是不是常量"只能在语义阶段判 —— 这正是本函数存在的原因。
//   ★ 实测：局部数组的维度可以引用**全局符号常量**
//     （63_big_int_mul.sy: `const int len = 20;` … `int c1[len + 5];`），
//     所以维度求值走 ConstEvaluator 的符号常量查表。
// ============================================================================
const Type* Sema::buildArrayType(BType base, const std::vector<Dim>& dims,
                                 bool isArrayParam) {
  TypeContext& t = typeContext();
  const Type* scalar = (base == BType::Int) ? t.intType() : t.floatType();
  if (dims.empty()) return scalar;

  std::vector<int64_t> lens(dims.size(), 1);
  for (size_t i = 0; i < dims.size(); ++i) {
    const bool firstOfParam = isArrayParam && i == 0;
    if (firstOfParam) {
      // 形参第一维**必须**为空（规范 §3 FuncFParam 2 的文法就是 `['[' ']']`）
      if (dims[i].expr != nullptr) {
        error(dims[i].loc, D::kArrayDim,
              "形参数组的第一维必须写成 `[]`（规范 §3 FuncFParam 2）");
      }
      lens[i] = kUnknownDim;
      continue;
    }
    if (dims[i].expr == nullptr) {
      int64_t dummy = 0;
      eval_.evalDim(nullptr, dummy, D::kArrayDim);   // 报"必须显式给出长度"
      lens[i] = 1;
      continue;
    }
    int64_t v = 0;
    if (!eval_.evalDim(dims[i].expr.get(), v, D::kArrayDim)) v = 1;   // 已报错
    lens[i] = v;
  }

  // 由内向外构造：`int a[2][3]` ≡ Array(Array(Int,3), 2)
  const Type* ty = scalar;
  for (size_t i = lens.size(); i-- > 0;) ty = typeContext().arrayOf(ty, lens[i]);
  return ty;
}

}  // namespace sysy
