// ============================================================================
// ir/Type.cpp —— 类型池与类型文本（`ir/Type.h` 的实现）
//
//   本文件是 S05 的 `sir::TypePool` / `appendTypeText` / `parseTypeText` /
//   `typeByteSize` / `typeElem` 的**完整搬迁**（一行逻辑都没改，只换了命名空间
//   并补了一条溢出保护），因为两层用的是同一套类型记号（见 Type.h 的文件头）。
// ============================================================================
#include "ir/Type.h"

namespace sysy {
namespace flat {

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

int64_t typeElemCount(const Type* t) {
  int64_t n = 1;
  const Type* c = t;
  while (c != nullptr && c->kind == TypeKind::Array) {
    if (c->len < 0) return -1;
    if (c->len != 0 && n > (INT64_MAX / c->len)) return -1;   // 溢出 ⇒ 不可知
    n *= c->len;
    c = c->elem;
  }
  return n;
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
      if (n < 0) return -1;
      // ★ 溢出保护：`int a[65536][65536]` 的字节数超过 int64。
      //   静默回绕会让"零初始化的大小"变成一个很小的正数（S04 抓过同类问题），
      //   所以这里显式返回 -1，由调用方决定（报诊断而不是算错）。
      if (t->len != 0 && n > (INT64_MAX / t->len)) return -1;
      return n * t->len;
    }
  }
  return 0;
}

}  // namespace flat
}  // namespace sysy
