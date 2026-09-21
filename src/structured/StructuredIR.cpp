// ============================================================================
// StructuredIR.cpp —— 容器的原语实现（**不含业务逻辑**）
//   规格与"为什么"见 StructuredIR.h 的文件头。
// ============================================================================
#include "structured/StructuredIR.h"

#include <cstring>

namespace sysy {
namespace sir {
namespace {

// OpKind ↔ 文本拼写。**唯一实现**：dump（StructuredDump.cpp）与读回
// （StructuredReader.cpp）都调这里，于是"印刷"与"识别"不可能漂移。
// 顺序与 StructuredIR.h 的 enum 一一对应（下面的 static_assert 兜底）。
struct KindName {
  OpKind kind;
  const char* name;
};
const KindName kKindNames[] = {
    {OpKind::Module, "Module"},       {OpKind::GlobalVar, "GlobalVar"},
    {OpKind::Func, "Func"},           {OpKind::GetArg, "GetArg"},
    {OpKind::Return, "Return"},       {OpKind::Call, "Call"},
    {OpKind::For, "For"},             {OpKind::While, "While"},
    {OpKind::If, "If"},               {OpKind::Goto, "Goto"},
    {OpKind::Yield, "Yield"},         {OpKind::Break, "Break"},
    {OpKind::Alloca, "Alloca"},       {OpKind::Load, "Load"},
    {OpKind::Store, "Store"},         {OpKind::GetElementPtr, "GetElementPtr"},
    {OpKind::Bitcast, "Bitcast"},     {OpKind::GetGlobal, "GetGlobal"},
    {OpKind::AddI, "AddI"},
    {OpKind::SubI, "SubI"},           {OpKind::MulI, "MulI"},
    {OpKind::DivI, "DivI"},           {OpKind::ModI, "ModI"},
    {OpKind::MinusI, "MinusI"},       {OpKind::AddF, "AddF"},
    {OpKind::SubF, "SubF"},           {OpKind::MulF, "MulF"},
    {OpKind::DivF, "DivF"},           {OpKind::MinusF, "MinusF"},
    {OpKind::Eq, "Eq"},               {OpKind::Ne, "Ne"},
    {OpKind::Lt, "Lt"},               {OpKind::Le, "Le"},
    {OpKind::Gt, "Gt"},               {OpKind::Ge, "Ge"},
    {OpKind::I2F, "I2F"},             {OpKind::F2I, "F2I"},
    {OpKind::Sext, "Sext"},           {OpKind::Int, "Int"},
    {OpKind::Float, "Float"},         {OpKind::Select, "Select"},
    {OpKind::Phi, "Phi"},             {OpKind::Unreachable, "Unreachable"},
};
constexpr size_t kNumKinds = sizeof(kKindNames) / sizeof(kKindNames[0]);
static_assert(kNumKinds == static_cast<size_t>(OpKind::Unreachable) + 1,
              "kKindNames 必须与 OpKind 一一对应（新增 OpKind 时同步此表）");

// 终结 Op（I4）：Yield / Break / Return / Goto / Unreachable 是全部。
// ⚠️ 为什么 `Break` 是终结 Op 而不是普通语句：设计文档 §1.2 把它列为
//    "循环体终结"。结构化层里"跳出循环"就是**当前 Region 到此为止**，
//    这正是 I4 能成为构造保证的原因。
bool isTerm(OpKind k) {
  switch (k) {
    case OpKind::Yield:
    case OpKind::Break:
    case OpKind::Return:
    case OpKind::Goto:
    case OpKind::Unreachable:
      return true;
    default:
      return false;
  }
}

// 控制流容器（I5）：只有这三种 Op 允许带 Region。
bool isCf(OpKind k) {
  return k == OpKind::For || k == OpKind::While || k == OpKind::If;
}

}  // namespace

const char* opKindName(OpKind k) {
  for (const KindName& e : kKindNames) {
    if (e.kind == k) return e.name;
  }
  return "?";
}

bool opKindFromName(const std::string& name, OpKind& out) {
  for (const KindName& e : kKindNames) {
    if (name == e.name) { out = e.kind; return true; }
  }
  return false;
}

bool isTerminator(OpKind k) { return isTerm(k); }
bool isControlFlowContainer(OpKind k) { return isCf(k); }

// ============================================================================
// TypePool / 类型文本
// ============================================================================
const Type* TypePool::voidTy() {
  for (const Type* t : pool_) {
    if (t->kind == TypeKind::Void) return t;
  }
  pool_.push_back(new Type{TypeKind::Void, nullptr, 0});
  return pool_.back();
}
const Type* TypePool::i32() {
  for (const Type* t : pool_) {
    if (t->kind == TypeKind::I32) return t;
  }
  pool_.push_back(new Type{TypeKind::I32, nullptr, 0});
  return pool_.back();
}
const Type* TypePool::i64() {
  for (const Type* t : pool_) {
    if (t->kind == TypeKind::I64) return t;
  }
  pool_.push_back(new Type{TypeKind::I64, nullptr, 0});
  return pool_.back();
}
const Type* TypePool::f32() {
  for (const Type* t : pool_) {
    if (t->kind == TypeKind::F32) return t;
  }
  pool_.push_back(new Type{TypeKind::F32, nullptr, 0});
  return pool_.back();
}
const Type* TypePool::ptrTo(const Type* elem) {
  for (const Type* t : pool_) {
    if (t->kind == TypeKind::Ptr && t->elem == elem) return t;
  }
  pool_.push_back(new Type{TypeKind::Ptr, elem, 0});
  return pool_.back();
}
const Type* TypePool::arrayOf(const Type* elem, int64_t len) {
  for (const Type* t : pool_) {
    if (t->kind == TypeKind::Array && t->elem == elem && t->len == len) return t;
  }
  pool_.push_back(new Type{TypeKind::Array, elem, len});
  return pool_.back();
}

TypePool& typePool() {
  static TypePool pool;
  return pool;
}

void appendTypeText(std::string& out, const Type* t) {
  if (t == nullptr) { out += "?"; return; }
  switch (t->kind) {
    case TypeKind::Void: out += "void"; return;
    case TypeKind::I32:  out += "i32";  return;
    case TypeKind::I64:  out += "i64";  return;
    case TypeKind::F32:  out += "f32";  return;
    case TypeKind::Ptr:
      out += "ptr[";
      appendTypeText(out, t->elem);
      out += ']';
      return;
    case TypeKind::Array:
      out += '[';
      out += std::to_string(t->len);
      out += " x ";
      appendTypeText(out, t->elem);
      out += ']';
      return;
  }
}

std::string typeText(const Type* t) {
  std::string s;
  appendTypeText(s, t);
  return s;
}

// 读回用的类型解析（`appendTypeText` 的逆函数；**递归深度由文本的括号层数
// 决定**，而 [N x T] 的层数 = 数组维数 ≤ 语料实测的 19，不是深树）。
const Type* parseTypeText(const std::string& s, size_t& pos) {
  auto skipWs = [&]() {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
  };
  skipWs();
  if (pos >= s.size()) return nullptr;
  if (s.compare(pos, 3, "i32") == 0) { pos += 3; return typePool().i32(); }
  if (s.compare(pos, 3, "i64") == 0) { pos += 3; return typePool().i64(); }
  if (s.compare(pos, 3, "f32") == 0) { pos += 3; return typePool().f32(); }
  if (s.compare(pos, 4, "void") == 0) { pos += 4; return typePool().voidTy(); }
  if (s.compare(pos, 4, "ptr[") == 0) {
    pos += 4;
    const Type* elem = parseTypeText(s, pos);
    if (elem == nullptr) return nullptr;
    skipWs();
    if (pos >= s.size() || s[pos] != ']') return nullptr;
    ++pos;
    return typePool().ptrTo(elem);
  }
  if (s[pos] == '[') {
    ++pos;
    skipWs();
    size_t b = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    if (pos == b) return nullptr;
    const int64_t n = std::stoll(s.substr(b, pos - b));
    skipWs();
    if (pos >= s.size() || s[pos] != 'x') return nullptr;
    ++pos;
    const Type* elem = parseTypeText(s, pos);
    if (elem == nullptr) return nullptr;
    skipWs();
    if (pos >= s.size() || s[pos] != ']') return nullptr;
    ++pos;
    return typePool().arrayOf(elem, n);
  }
  return nullptr;
}

const Type* typeElem(const Type* t) {
  if (t == nullptr || !t->isArray()) return t;
  return t->elem;
}

int64_t typeByteSize(const Type* t) {
  if (t == nullptr) return 0;
  switch (t->kind) {
    case TypeKind::Void: return 0;
    case TypeKind::I32:
    case TypeKind::F32:  return 4;
    case TypeKind::I64:  return 8;
    case TypeKind::Ptr:  return 8;    // 两种赛道的目标机都是 64 位
    case TypeKind::Array: {
      const int64_t n = typeByteSize(t->elem);
      return n < 0 ? -1 : n * t->len;
    }
  }
  return 0;
}

// ============================================================================
// Attr
// ============================================================================
Attr Attr::ofStr(std::string v) { Attr a; a.kind = Kind::Str; a.s = std::move(v); return a; }
Attr Attr::ofType(const Type* t) { Attr a; a.kind = Kind::Type; a.ty = t; return a; }
Attr Attr::ofInt(int64_t v) { Attr a; a.kind = Kind::Int; a.i = v; return a; }
Attr Attr::ofFBits(uint32_t bits) { Attr a; a.kind = Kind::Float; a.fbits = bits; return a; }

// ============================================================================
// Op
// ============================================================================
Value Op::addResult(const Type* type) {
  Result r;
  r.definer = this;
  r.index = static_cast<uint32_t>(results_.size());
  r.type = type;
  results_.push_back(r);
  return &results_.back();
}

// 下面三个"越界取默认值"的查询是**读回畸形输入**时的安全网（C5：不用 assert
// 代替错误处理）。读回器自己会报诊断；这里只保证不崩。
const std::string& Op::strAttr(size_t i) const {
  static const std::string kEmpty;
  const Attr* a = attr(i);
  return (a != nullptr && a->kind == Attr::Kind::Str) ? a->s : kEmpty;
}
int64_t Op::intAttr(size_t i) const {
  const Attr* a = attr(i);
  return (a != nullptr && a->kind == Attr::Kind::Int) ? a->i : 0;
}
const Type* Op::typeAttr(size_t i) const {
  const Attr* a = attr(i);
  return (a != nullptr && a->kind == Attr::Kind::Type) ? a->ty : nullptr;
}

// ============================================================================
// Arena
// ============================================================================
Op* Arena::makeOp(OpKind k, SourceLoc loc) {
  Op* op = new Op(k);
  op->loc = loc;
  ops_.push_back(op);
  return op;
}

Region* Arena::makeRegion() {
  Region* r = new Region();
  regions_.push_back(r);
  return r;
}

void Arena::clear() {
  // **迭代**释放：Op 只存 Region/Op 的**裸指针**（不持有所有权），所以逐个
  // delete 不会向下递归；Region 的析构只释放自己的 vector<Op*>（指针，不递归）。
  // ⇒ 4000 层深的 IR 也不会在析构时爆栈（S02 的 destroyTree 教训）。
  for (Op* op : ops_) delete op;
  ops_.clear();
  for (Region* r : regions_) delete r;
  regions_.clear();
}

// ============================================================================
// 遍历原语（显式工作栈）
// ============================================================================
void forEachOp(Op* root, const std::function<void(Op*)>& f) {
  if (root == nullptr) return;
  std::vector<Op*> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    Op* op = stack.back();
    stack.pop_back();
    if (op == nullptr) continue;
    f(op);
    // 反序压栈 ⇒ 弹出顺序 = 先序（Region 按序、Region 内按序）。
    for (size_t i = op->regions().size(); i-- > 0;) {
      Region* r = op->regions()[i];
      if (r == nullptr) continue;
      const std::vector<Op*>& ops = r->ops();
      for (size_t j = ops.size(); j-- > 0;) stack.push_back(ops[j]);
    }
  }
}

size_t countOps(Op* root, OpKind k) {
  size_t n = 0;
  forEachOp(root, [&](Op* op) { if (op->kind == k) ++n; });
  return n;
}

}  // namespace sir
}  // namespace sysy
