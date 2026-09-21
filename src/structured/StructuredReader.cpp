// ============================================================================
// StructuredReader.cpp —— 结构化 IR 文本 → 容器（`dumpModule` 的逆函数）
//
//   ★ 存在的唯一理由：**轨 A**（prompt §七）——
//     `--emit=structured-ir` → `--from-structured --emit=structured-ir`
//     → **逐字节相同**。它证明"打印器与读取器互逆"。
//
// ── 两趟解析：为什么必须这样（一次实测的教训）─────────────────────────────
//   第一版用"单趟 + 边读边解析操作数"，结果卡在一个**本质歧义**上：
//     `(If %x @line 5) {` 里的 `%x` 既可能是**结果名**、也可能是**条件操作数**
//     （而它在文本上排在 Region 之前 ⇒ 无法用位置判定）。
//   教训：**格式本身不许有歧义**。所以最终形态是：
//     * `IntOp`/`FloatOp` 的值是**字面量**（`(Int %f.0 5)`）；
//     * **其余操作数一律是结果名**（`%f.0`），常量也走名字；
//     * `Int`/`Float`/`GetGlobal` 是**有结果的定义**，`Store`/`Return`/`If`/… 没有；
//       ⇒ "一行里第一个记号是不是 `%`"完全由 OpKind 决定（见 `arityOf`）。
//   于是**单趟**就能把树建好，只是操作数的"名字 → 定义"解析要放到**第二趟**
//   （因为 `While`/`If` 的条件操作数是**先定义后使用**吗？——不！条件操作数写在
//    Region **之前**，而它的定义在 Region **里面**。所以必须两趟。）
// ============================================================================
#include "structured/StructuredReader.h"

#include "structured/StructuredReaderLex.h"

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "frontend/RuntimeLib.h"
#include "support/Diagnostic.h"

namespace sysy {
namespace sir {
namespace {

// 说明：词法层（`TK`/`Tok`/`Line`/`lexLine`/`parseI32`/`parseF32Bits`）在
// `StructuredReaderLex.h`。下面用 `using` 把它们引进本命名空间（同一份实现）。
using sir::Line;
using sir::lexLine;
using sir::parseF32Bits;
using sir::parseI32;
using sir::TK;
using sir::Tok;

// 【后置】这个被调名字是不是"返回 void 的内建/运行时符号"。规则是**通用的**
//   （"名字表"性质），不是按名字写特判：
//     * `_sysy<X>`：运行时名字表里 `<X>` 是零参 void 函数（`starttime`/`stoptime`）
//     * `llvm.memcpy` / `llvm.memset`：仅有的两个内建（docs/handoff/iset.txt）
bool isVoidBuiltin(const std::string& callee) {
  if (callee.rfind("_sysy", 0) == 0) return true;
  if (callee == "llvm.memcpy" || callee == "llvm.memset") return true;
  // ★ 还要查**运行时库签名表**：`putint`/`putch`/`putfloat`/`putarray`/
  //   `putfarray` 是 SysY 层的 void 函数，dump 里是被调名字。
  //   漏了这一步 ⇒ 读回器认为它们有结果 ⇒ 结果序号计数器错位 1
  //   （实测：`matmul1` 的往返在 440 行开始整段偏移）。
  //   `lookupRuntimeFunc` 是**名字表查询**，不是"按名字写特判"（铁律 2 允许）。
  const FuncSig* sig = lookupRuntimeFunc(callee);
  return sig != nullptr && isVoid(sig->ret);
}

// ── 每个 OpKind 的**结果个数**（0 或 1；`Call` 由被调函数决定）──────────────
//   这是"单趟建树"能成立的前提：一行里第一个记号是不是 `%` 完全由 OpKind 决定。
struct Arity {
  bool hasResult;
  bool byCallee;   // true = 看被调函数是否是 void
};
Arity arityOf(OpKind k) {
  switch (k) {
    case OpKind::Store: case OpKind::Return: case OpKind::Goto:
    case OpKind::Yield: case OpKind::Break: case OpKind::Unreachable:
    case OpKind::Func: case OpKind::Module:
    case OpKind::GlobalVar:   // 模块级命名实体（由 GetGlobalOp 引用）⇒ 无结果
    case OpKind::If:          // 纯语句：条件在操作数里、两个 Region 是分支
    case OpKind::While:       // ★ S05 不产 ForOp；While/For **都没有结果**
    case OpKind::For:
      return Arity{false, false};
    case OpKind::Call:
      // ★ `hasResult` 必须是 **false**：Call 的结果个数由**被调函数**决定，
      //   而"被调是谁"要先读名字（`byCallee` 分支负责）。
      //   写成 {true,true} 会让 void 的内建调用也去读结果 —— 实测报"缺少结果名"。
      return Arity{false, true};
    default:
      return Arity{true, false};
  }
}

// ============================================================================
// 1. 类型解析：从记号流里读**恰好一个**类型
//    `i32` / `i64` / `f32` / `void` / `ptr[T]` / `[N x T]`
// ============================================================================
const Type* parseTyRec(const std::vector<Tok>& t, size_t& i);
const Type* parseArrayTy(const std::vector<Tok>& t, size_t& i);

const Type* parseTyRec(const std::vector<Tok>& t, size_t& i) {
  if (i >= t.size() || t[i].k != TK::Ident) return nullptr;
  const std::string head = t[i].s;
  if (head == "i32") { ++i; return typePool().i32(); }
  if (head == "i64") { ++i; return typePool().i64(); }
  if (head == "f32") { ++i; return typePool().f32(); }
  if (head == "void") { ++i; return typePool().voidTy(); }
  if (head == "ptr") {
    ++i;
    if (i >= t.size() || t[i].k != TK::LBracket) return nullptr;
    ++i;
    const Type* elem = (i < t.size() && t[i].k == TK::LBracket) ? parseArrayTy(t, i)
                                                               : parseTyRec(t, i);
    if (elem == nullptr) return nullptr;
    if (i >= t.size() || t[i].k != TK::RBracket) return nullptr;
    ++i;
    return typePool().ptrTo(elem);
  }
  return nullptr;
}

const Type* parseArrayTy(const std::vector<Tok>& t, size_t& i) {
  if (i >= t.size() || t[i].k != TK::LBracket) return nullptr;
  ++i;
  if (i >= t.size() || t[i].k != TK::Num) return nullptr;
  const int64_t n = std::strtoll(t[i].s.c_str(), nullptr, 10);
  ++i;
  if (i >= t.size() || t[i].k != TK::Ident || t[i].s != "x") return nullptr;
  ++i;
  const Type* elem = (i < t.size() && t[i].k == TK::LBracket) ? parseArrayTy(t, i)
                                                             : parseTyRec(t, i);
  if (elem == nullptr) return nullptr;
  if (i >= t.size() || t[i].k != TK::RBracket) return nullptr;
  ++i;
  return typePool().arrayOf(elem, n);
}

const Type* parseTy(const std::vector<Tok>& t, size_t& i) {
  if (i < t.size() && t[i].k == TK::Colon) ++i;   // 容错：允许前导 `:`
  if (i < t.size() && t[i].k == TK::LBracket) return parseArrayTy(t, i);
  return parseTyRec(t, i);
}

// ============================================================================
// 2. Reader
// ============================================================================
class Reader {
 public:
  Reader(Arena& a, DiagnosticEngine& d) : arena_(a), diag_(d) {}
  Op* run(const std::string& text);

 private:
  void err(size_t line, const std::string& msg) {
    diag_.report(DiagLevel::Error, SourceLoc(static_cast<uint32_t>(line), 1),
                 "E-STRUCT-READ", msg);
  }
  struct P {
    const std::vector<Tok>& t;
    size_t i = 0;
    bool at(TK k) const { return i < t.size() && t[i].k == k; }
    bool eof() const { return i >= t.size(); }
    const Tok& peek() const { static const Tok kEmpty; return i < t.size() ? t[i] : kEmpty; }
    bool accept(TK k) { if (at(k)) { ++i; return true; } return false; }
  };

  // 【后置】解析一行 Op（建树；操作数只记名字，第二趟解析）
  Op* parseOp(const Line& ln, size_t lineNo);
  bool parseHead(P& p, Op* op, OpKind kind, size_t lineNo);
  const Type* resultTypeOf(const Op* op, OpKind kind, const std::string& nm);

  Arena& arena_;
  DiagnosticEngine& diag_;
  std::unordered_map<std::string, Value> bind_;   // 结果名 → Result*
  std::unordered_map<std::string, const Type*> fnRet_;   // 函数名 → 返回类型
  std::string curFunc_;
  uint32_t counter_ = 0;
  // 第二趟要解析的操作数：`(Op*, names...)`
  std::vector<std::pair<Op*, std::vector<std::string>>> pending_;
};

bool Reader::parseHead(P& p, Op* op, OpKind kind, size_t lineNo) {
  auto needStr = [&](const char* what) {
    if (!p.at(TK::Str)) { err(lineNo, std::string("缺少") + what); return false; }
    return true;
  };
  // 头部属性里"名字"的位置**按 OpKind 固定**（都在结果之后、操作数之前）：
  //   Module/Func/Call/GetGlobal/GlobalVar 的第 1 个属性是名字字符串。
  //   ⚠️ 这条顺序必须与 StructuredDump.cpp 逐字对应，否则 `strAttr(0)` 会是空串
  //      （实测：Call 少登记了名字 ⇒ void 判定失效 ⇒ 报"缺少结果名"）。
  //   ⚠️ GetGlobal 这里**只吃名字**：结果已经在 parseOp 里吃过了。
  // 名字在这三个 Op 的**结果之后**（`Call`/`GetGlobal` 的名字已在 parseOp 里读过）
  if (kind == OpKind::Module || kind == OpKind::Func || kind == OpKind::GlobalVar) {
    if (!needStr("名字字符串")) return false;
    op->addAttr(Attr::ofStr(p.peek().s));
    ++p.i;
  }
  if (kind == OpKind::Func) {
    // `:ret <ty>` 与 `:param [ty, ty]`（prompt §六：函数头必须带签名）
    if (!(p.at(TK::Colon) && p.i + 1 < p.t.size() && p.t[p.i + 1].s == "ret")) {
      err(lineNo, "Func 缺少 `:ret`");
      return false;
    }
    p.i += 2;
    const Type* rt = parseTy(p.t, p.i);
    if (rt == nullptr) { err(lineNo, "`:ret` 的类型无法解析"); return false; }
    op->addAttr(Attr::ofType(rt));
    fnRet_[op->strAttr(0)] = rt;
    if (!(p.at(TK::Colon) && p.i + 1 < p.t.size() && p.t[p.i + 1].s == "param")) {
      err(lineNo, "Func 缺少 `:param`");
      return false;
    }
    p.i += 2;
    if (!p.accept(TK::LBracket)) { err(lineNo, "`:param` 后面必须是 `[`"); return false; }
    while (!p.at(TK::RBracket) && !p.eof()) {
      const Type* pt = parseTy(p.t, p.i);
      if (pt == nullptr) { err(lineNo, "形参类型无法解析"); return false; }
      op->addAttr(Attr::ofType(pt));
      if (!p.accept(TK::Comma)) break;
    }
    if (!p.accept(TK::RBracket)) { err(lineNo, "`:param [` 没有闭合"); return false; }
    return true;
  }
  if (kind == OpKind::GlobalVar || kind == OpKind::GetGlobal) {
    if (!(p.at(TK::Colon) && p.i + 1 < p.t.size() && p.t[p.i + 1].s == "type")) {
      err(lineNo, "缺少 `:type`");
      return false;
    }
    p.i += 2;
    const Type* ty = parseTy(p.t, p.i);
    if (ty == nullptr) { err(lineNo, "类型无法解析"); return false; }
    op->addAttr(Attr::ofType(ty));
    if (kind == OpKind::GetGlobal) return true;
    if (!(p.at(TK::Colon) && p.i + 1 < p.t.size() && p.t[p.i + 1].s == "init")) {
      err(lineNo, "GlobalVar 缺少 `:init`");
      return false;
    }
    p.i += 2;
    if (!needStr("`:init` 的值")) return false;
    op->addAttr(Attr::ofStr(p.peek().s));
    ++p.i;
    if (p.peek().k == TK::LBrace) {   // `:data {off=val, ...}`
      ++p.i;
      Attr data;
      data.kind = Attr::Kind::Data;
      while (!p.at(TK::RBrace) && !p.eof()) {
        if (!p.at(TK::Num)) { err(lineNo, "data 表的偏移不是数字"); return false; }
        int32_t off = 0;
        if (!parseI32(p.peek().s, off)) { err(lineNo, "data 偏移无法解析"); return false; }
        ++p.i;
        if (!p.accept(TK::Equals)) { err(lineNo, "data 表缺少 `=`"); return false; }
        if (!p.at(TK::Num)) { err(lineNo, "data 表的值不是数字"); return false; }
        int32_t v = 0;
        if (!parseI32(p.peek().s, v)) { err(lineNo, "data 值无法解析"); return false; }
        ++p.i;
        data.data.emplace_back(static_cast<uint64_t>(static_cast<uint32_t>(off)),
                               static_cast<uint32_t>(v));
        if (!p.accept(TK::Comma)) break;
      }
      if (!p.accept(TK::RBrace)) { err(lineNo, "data 表没有闭合"); return false; }
      op->addAttr(std::move(data));
    }
    return true;
  }
  return true;   // 其余 Op 没有头部属性
}

Op* Reader::parseOp(const Line& ln, size_t lineNo) {
  P p{ln.toks};
  if (!p.accept(TK::LParen)) { err(lineNo, "Op 行必须以 `(` 开头"); return nullptr; }
  if (!p.at(TK::Ident)) { err(lineNo, "缺少 OpKind"); return nullptr; }
  OpKind kind{};
  const std::string kindName = p.peek().s;
  if (!opKindFromName(kindName, kind)) {
    err(lineNo, "未知 OpKind `" + kindName + "`（指令集不封闭）");
    return nullptr;
  }
  ++p.i;
  Op* op = arena_.makeOp(kind, SourceLoc());
  if (kind == OpKind::Func) { curFunc_ = p.i < p.t.size() ? p.t[p.i].s : ""; counter_ = 0; }

  // ⓪ `Call` / `GetGlobal` 的**名字在结果之前**：
  //     `(GetGlobal "g" %.0 :type ptr[i32])` / `(Call "f" %f.3 %a @line 5)`。
  //     `Call` 尤其必须如此：被调函数决定"有没有结果"。
  if (kind == OpKind::Call || kind == OpKind::GetGlobal) {
    if (!p.at(TK::Str)) { err(lineNo, "缺少名字字符串（Call/GetGlobal）"); return nullptr; }
    op->addAttr(Attr::ofStr(p.peek().s));
    ++p.i;
  }

  // ① 结果 —— 个数由 OpKind 决定；**唯一的例外是 `Call`**（由被调函数决定，
  //    而名字刚刚读到 ⇒ 在这里才判定）。
  //   ⚠️ 条件里**不能**写 `&& hasResult`：`arityOf(Call).hasResult` 就是 false，
  //      那样写会跳过判定 ⇒ 所有非 void 的调用都"没有结果"（实测）。
  const Arity ar = arityOf(kind);
  bool hasResult = ar.hasResult;
  if (ar.byCallee) {
    const std::string callee = op->strAttr(0);
    const auto it = fnRet_.find(callee);
    const Type* rt = (it != fnRet_.end()) ? it->second
                                          : (isVoidBuiltin(callee) ? typePool().voidTy()
                                                                   : typePool().i32());
    hasResult = (rt->kind != TypeKind::Void);
  }
  if (hasResult) {
    if (!p.at(TK::Percent)) { err(lineNo, "缺少结果名"); return nullptr; }
    ++p.i;
    if (!p.at(TK::Ident)) { err(lineNo, "结果名缺少数值部分"); return nullptr; }
    const std::string nm = "%" + p.peek().s;
    ++p.i;
    bind_[nm] = op->addResult(resultTypeOf(op, kind, nm));
    ++counter_;
  }

  // ③ 头部属性（`Call`/`GetGlobal` 的名字已在上面读过）
  if (!parseHead(p, op, kind, lineNo)) return nullptr;

  // ② 子 Region（按 OpKind 的规格**先建好**，`{` 只表示"内容从这里开始"）
  switch (kind) {
    case OpKind::Module:
    case OpKind::Func:
    case OpKind::For:
      op->addRegion(arena_.makeRegion());
      break;
    case OpKind::While:
    case OpKind::If:
      op->addRegion(arena_.makeRegion());
      op->addRegion(arena_.makeRegion());
      break;
    default:
      break;
  }

  // ③ 操作数：`Int`/`Float` 是字面量，其余一律引用结果名
  std::vector<std::string> ops;
  if (kind == OpKind::Int || kind == OpKind::Float) {
    if (!p.at(TK::Num)) { err(lineNo, "常量定义缺少数值"); return nullptr; }
    const std::string lit = p.peek().s;
    ++p.i;
    if (kind == OpKind::Float) {
      uint32_t bits = 0;
      if (!parseF32Bits(lit, bits)) { err(lineNo, "f32 常量无法解析：" + lit); return nullptr; }
      op->addAttr(Attr::ofFBits(bits));
    } else {
      int32_t v = 0;
      if (!parseI32(lit, v)) { err(lineNo, "i32 常量无法解析：" + lit); return nullptr; }
      op->addAttr(Attr::ofInt(v));
    }
    // 常量定义同样可以带 `@line`
    if (p.at(TK::AtLine)) {
      ++p.i;
      if (!p.at(TK::Num)) { err(lineNo, "@line 后面不是数字"); return nullptr; }
      int32_t v = 0;
      if (!parseI32(p.peek().s, v)) { err(lineNo, "@line 的值无法解析"); return nullptr; }
      ++p.i;
      op->loc = SourceLoc(static_cast<uint32_t>(v), 1);
    }
  } else {
    while (!p.eof() && !p.at(TK::RParen)) {
      if (p.at(TK::Percent)) {
        ++p.i;
        if (!p.at(TK::Ident)) { err(lineNo, "操作数名缺少数值部分"); return nullptr; }
        ops.push_back("%" + p.peek().s);
        ++p.i;
        continue;
      }
      if (p.at(TK::AtLine)) {
        ++p.i;
        if (!p.at(TK::Num)) { err(lineNo, "@line 后面不是数字"); return nullptr; }
        int32_t v = 0;
        if (!parseI32(p.peek().s, v)) { err(lineNo, "@line 的值无法解析"); return nullptr; }
        ++p.i;
        op->loc = SourceLoc(static_cast<uint32_t>(v), 1);
        continue;
      }
      // 无名的类型属性（只有 GetElementPtr 有：元素类型 + 下标类型）
      if (p.at(TK::Ident) || p.at(TK::LBracket)) {
        const Type* ty = parseTy(p.t, p.i);
        if (ty == nullptr) { err(lineNo, "类型属性无法解析"); return nullptr; }
        op->addAttr(Attr::ofType(ty));
        continue;
      }
      // 裸整数属性（只有 GetElementPtr 的亲和性标记）
      if (p.at(TK::Num)) {
        int32_t v = 0;
        if (!parseI32(p.peek().s, v)) {
          err(lineNo, "整数属性无法解析：[" + p.peek().s + "]");
          return nullptr;
        }
        ++p.i;
        op->addAttr(Attr::ofInt(v));
        continue;
      }
      err(lineNo, "无法识别的记号：[" + p.peek().s + "]");
      return nullptr;
    }
  }
  if (!p.accept(TK::RParen)) {
    std::string all;
    for (const Tok& tk : ln.toks) all += "[" + tk.s + "]";
    err(lineNo, "Op 行没有闭合的 `)` tok=[" + p.peek().s + "] line=" + all);
    return nullptr;
  }
  pending_.emplace_back(op, std::move(ops));
  return op;
}

// 【后置】按 OpKind 推出结果类型。
const Type* Reader::resultTypeOf(const Op* op, OpKind kind, const std::string& nm) {
  (void)nm;
  auto at = [&](size_t i) { return op->typeAttr(i); };
  switch (kind) {
    case OpKind::Alloca:        return typePool().ptrTo(at(0));
    case OpKind::GetGlobal:     return typePool().ptrTo(at(1));
    case OpKind::Load:          return at(0);
    case OpKind::GetElementPtr: return typePool().ptrTo(at(0));
    case OpKind::Bitcast:       return at(0);
    case OpKind::Call: {
      const auto it = fnRet_.find(op->strAttr(0));
      if (it != fnRet_.end()) return it->second;
      if (isVoidBuiltin(op->strAttr(0))) return typePool().voidTy();
      return typePool().i32();
    }
    case OpKind::Int:           return typePool().i32();
    case OpKind::Float:         return typePool().f32();
    case OpKind::AddI: case OpKind::SubI: case OpKind::MulI:
    case OpKind::DivI: case OpKind::ModI: case OpKind::MinusI:
    case OpKind::F2I: case OpKind::Eq: case OpKind::Ne:
    case OpKind::Lt: case OpKind::Le: case OpKind::Gt: case OpKind::Ge:
    case OpKind::GetArg:
      return typePool().i32();
    case OpKind::AddF: case OpKind::SubF: case OpKind::MulF:
    case OpKind::DivF: case OpKind::MinusF: case OpKind::I2F:
      return typePool().f32();
    case OpKind::Sext: return typePool().i64();
    case OpKind::Select: case OpKind::Phi:
      return (op->numOperands() > 0 && op->operand(0) != nullptr) ? op->operand(0)->type
                                                                 : typePool().i32();
    default: return typePool().i32();
  }
}

Op* Reader::run(const std::string& text) {
  // ── ① 切行 + 词法 ──────────────────────────────────────────────────────
  std::vector<Line> lines;
  {
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i <= text.size(); ++i) {
      const char c = (i < text.size()) ? text[i] : '\n';
      if (c == '"' && (i == 0 || text[i - 1] != '\\')) inStr = !inStr;
      if (!inStr && c == '\n') {
        if (!cur.empty()) {
          Line ln;
          if (!lexLine(cur, ln)) {
            err(0, "无法识别的字符（读回器不认识这一行）");
            return nullptr;
          }
          if (!ln.toks.empty() || ln.openBrace || ln.closeBraceFirst) lines.push_back(ln);
        }
        cur.clear();
        continue;
      }
      if (c != '\r') cur += c;
    }
  }
  if (lines.empty()) { err(0, "结构化 IR 文本为空"); return nullptr; }

  // ── ② 第一趟：建树（Region 归属 + 结果绑定，不解析操作数）────────────
  //   `braceDepth_` 是**词法**的花括号深度（`{` 与 `}` 一一对应）；
  //   语义帧只在"Op 的最后一个子 Region 结束"时弹出 ⇒ 单独一行的 `{`（`IfOp`
  //   的第二个 Region）只改 `braceDepth_`，不改语义帧。
  // 【语义帧 vs 词法深度】一个 Op 的**每个**子 Region 都以一条独立的 `}` 收尾，
  //   而多 Region 的 Op（`IfOp`）的第 2..n 个 Region 还有**单独一行**的 `{`。
  //   所以"当前 Region"必须显式跟踪：`nextRegion` = 这个 Op 下一个要填的 Region
  //   下标（`nRegion` = 它的子 Region 总数）。**不能**靠"栈顶是谁"推断 ——
  //   实测那样写会让 `IfOp` 的 else 分支内容漏进 then 分支。
  struct Frame {
    Op* owner = nullptr;      // 拥有这些 Region 的 Op（模块帧为 nullptr）
    size_t nRegion = 0;
    size_t nextRegion = 0;
    int braceAt = 0;
  };
  std::vector<Frame> stack;
  int depth = 0;
  if (lines[0].closeBraceFirst) { err(1, "第一行是 `}`"); return nullptr; }
  Op* root = parseOp(lines[0], 1);
  if (root == nullptr) return nullptr;
  if (root->kind != OpKind::Module) { err(1, "第一行必须是 ModuleOp"); return nullptr; }
  if (lines[0].openBrace && root->numRegions() > 0) {
    depth = 1;
    // `braceAt` = **进入该 Region 之前的**深度（这个 Region 的 `}` 会回到它）
    stack.push_back(Frame{root, root->numRegions(), 0, 0});
  }
  for (size_t li = 1; li < lines.size(); ++li) {
    const Line& ln = lines[li];
    const size_t lineNo = li + 1;
    if (ln.closeBraceFirst) {
      --depth;
      if (depth < 0) { err(lineNo, "多余的 `}`"); return nullptr; }
      // 关掉一层：**只有"该 Op 的所有子 Region 都读完了"才弹帧**。
      //   ⚠️ 不能用"回到入口深度"判：`IfOp` 的 then Region 的 `}` 也回到入口深度，
      //      那时 else Region 还没读（实测：弹早了 ⇒ 后面全乱）。
      if (!stack.empty()) {
        Frame& top = stack.back();
        ++top.nextRegion;
        if (top.nextRegion >= top.nRegion) stack.pop_back();
      }
      continue;
    }
    if (ln.toks.empty()) {
      if (!ln.openBrace) continue;
      // 单独一行 `{`（多 Region 的第 2..n 个）⇒ 什么都不用做：
      //   "当前 Region" 由 `nextRegion`（**尚未开始**的那个）直接决定。
      ++depth;
      continue;
    }
    // 当前 Region = 栈顶 Op 的 `nextRegion`（**尚未开始**的那个子 Region）
    Region* host = nullptr;
    if (stack.empty()) {
      host = root->region(0);
    } else {
      Frame& top = stack.back();
      if (top.nextRegion >= top.nRegion) {
        err(lineNo, "Op 出现在所有子 Region 之外");
        return nullptr;
      }
      host = top.owner->region(top.nextRegion);
    }
    Op* op = parseOp(ln, lineNo);
    if (op == nullptr) return nullptr;
    host->push(op);
    if (ln.openBrace) {
      ++depth;
      if (op->numRegions() == 0) { err(lineNo, "`{` 出现在不带 Region 的 Op 上"); return nullptr; }
      stack.push_back(Frame{op, op->numRegions(), 0, depth - 1});
    }
  }
  if (depth != 0) { err(lines.size(), "花括号没有配平（缺少 `}`）"); return nullptr; }
  if (!stack.empty()) { err(lines.size(), "Region 没有闭合（缺少 `}`）"); return nullptr; }

  // ── ③ 第二趟：解析操作数（此时所有结果名都已绑定）──────────────────────
  for (auto& pr : pending_) {
    Op* op = pr.first;
    for (const std::string& nm : pr.second) {
      const auto it = bind_.find(nm);
      if (it == bind_.end()) {
        err(op->loc.line, "操作数 `" + nm + "` 没有定义（use-def 不一致）");
        op->addOperand(nullptr);
        continue;
      }
      op->addOperand(it->second);
    }
  }
  return root;
}

}  // namespace

Op* parseStructuredModule(const std::string& text, Arena& arena, DiagnosticEngine& diag) {
  Reader r(arena, diag);
  return r.run(text);
}

}  // namespace sir
}  // namespace sysy
