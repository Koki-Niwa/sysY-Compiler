// ============================================================================
// ir/FlatDump.cpp —— 平面 IR 文本 dump 的**唯一实现**（规则见 FlatDump.h）
//
// ── 编号（`%N` / `L<N>`）**只依赖最终的 IR 结构**（不依赖构造顺序）──────
//   这是轨 D（独立第二份实现）能逐字节比较的**前提**：若编号依赖"谁先被造
//   出来"，两份实现永远对不上，判据就只能放宽到"结构等价"——而放宽之后，
//   轨 D 抓不住的东西就多了一大类。
//   ⇒ 规则（写进设计文档 §4.1，两份实现都照它做）：
//     * 参数先发号（`%0`…`%n-1`）；
//     * 然后逐块（块序）、逐指令（指令序）给**有结果的指令**发号；
//       **常量**在**首次被引用**时发号（同一条指令内按操作数顺序）；
//     * 基本块号 = 块在函数块表里的下标（`L0` 恒为入口块）。
//
// ── 常量定义行的位置（一个必须写死的排版决定）────────────────────────────
//   常量的定义行插在**该函数第一条指令之前**、按编号升序。
//   理由是"定义在前、使用在后"在文本上可见（也符合 LLVM 的阅读习惯）；
//   代价是读回器要先扫一遍常量行再读块（见 FlatReader.cpp）。
// ============================================================================
#include "ir/FlatDump.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace sysy {
namespace flat {
namespace {

constexpr int kIndent = 2;   // 每层缩进 2 空格（与 S05 的 structured dump 一致）

// ── 十六进制浮点（`%a`）───────────────────────────────────────────────────
//   为什么不用十进制：`0.1f` 没有精确的十进制短表示，十进制往返会改变位模式
//   （轨 A 要求 dump→读回→dump **逐字节相同**，位模式变了就不成立）。
void appendHexFloat(std::string& out, uint32_t bits) {
  float f = 0.0f;
  static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
  std::memcpy(&f, &bits, sizeof(f));
  if (f != f) { out += "nan"; return; }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%a", static_cast<double>(f));
  out += buf;
}

void appendLine(std::string& out, uint32_t line) {
  if (line == 0) return;      // `@line 0` 不打印（中转块没有源码位置）
  out += " @line ";
  out += std::to_string(line);
}

// 编号表：值 → `%N`。
struct Numbering {
  std::unordered_map<const Value*, uint32_t> names;
  uint32_t next = 0;
  // 常量定义（按发号顺序）—— 用于在函数开头补打印它们的定义行。
  std::vector<const Instruction*> consts;
  // 全局地址的定义（按发号顺序）—— 同上（`%N = ptr[T] @g`）
  std::vector<const GlobalAddr*> globals;
};

std::string nameOf(const Numbering& n, const Value* v) {
  const auto it = n.names.find(v);
  return it == n.names.end() ? std::string("%?") : ("%" + std::to_string(it->second));
}

// 【后置】给 v 发号（若还没有），返回它的名字。
std::string nameFor(Numbering& n, const Value* v) {
  if (v == nullptr) return "%?";
  const auto it = n.names.find(v);
  if (it != n.names.end()) return "%" + std::to_string(it->second);
  const uint32_t id = n.next++;
  n.names[v] = id;
  // ★ 常量是**指令形态**（`Opcode::ConstantInt`/`ConstantFP`），不是
  //   `ValueKind::Constant` —— 后者是另一套（模块级 interned 常量）。
  //   第一版这里写成 `v->isConstant()`（值形态判定）⇒ 常量永远不进
  //   `n.consts`，dump 打成 `%?`（实测就是这个症状）。
  if (v->isInst()) {
    const auto* inst = static_cast<const Instruction*>(v);
    if (inst->isConstant()) n.consts.push_back(inst);
  } else if (v->isGlobalAddr()) {
    n.globals.push_back(static_cast<const GlobalAddr*>(v));
  }
  return "%" + std::to_string(id);
}

void printTypedValue(std::string& out, const Numbering& n, const Value* v) {
  out += typeText(v != nullptr ? v->type() : nullptr);
  out += ' ';
  out += nameOf(n, v);
}

// 【后置】`ret` / `br` / `unreachable` 三种终结符（4 种形式）。
void printTerminator(std::string& out, const Numbering& n, const Instruction* i) {
  switch (i->op()) {
    case Opcode::Ret:
      if (i->numOperands() == 0) {
        out += "ret void";
      } else {
        out += "ret ";
        printTypedValue(out, n, i->operand(0));
      }
      break;
    case Opcode::Br: {
      const BasicBlock* t = i->succ(0);
      const BasicBlock* f = i->succ(1);
      if (i->numSuccs() == 1 || f == nullptr) {
        out += "br L";
        out += std::to_string(t != nullptr ? t->index() : 0);
      } else {
        out += "br i1 ";
        out += nameOf(n, i->operand(0));
        out += ", label L";
        out += std::to_string(t->index());
        out += ", label L";
        out += std::to_string(f->index());
      }
      break;
    }
    case Opcode::Unreachable:
      out += "unreachable";
      break;
    default:
      out += "?";
      break;
  }
}

// 【后置】打印一条指令的正文（不含缩进与 `@line`）。
//   ⚠️ 与 `FlatReader.cpp` 的 `parseInst` **逐字对应**。
void printInst(std::string& out, const Numbering& n, const Instruction* i) {
  if (isTerminatorOpcode(i->op())) {
    printTerminator(out, n, i);
    return;
  }
  const size_t nops = i->numOperands();
  switch (i->op()) {
    // ── 常量（`%7 = i32 5` / `%8 = f32 0x1p+0`）─────────────────────────
    case Opcode::ConstantInt:
      out += typeText(i->type());
      out += ' ';
      out += std::to_string(i->intBits());
      return;
    case Opcode::ConstantFP:
      out += "f32 ";
      appendHexFloat(out, i->floatBits());
      return;
    // ── 内存 ──────────────────────────────────────────────────────────
    case Opcode::Alloca:
      out += "alloca ";
      out += typeText(i->srcElemType());
      return;
    case Opcode::Load:
      out += "load ";
      out += typeText(i->type());          // 元素类型 = 结果类型
      out += ", ";
      printTypedValue(out, n, i->operand(0));
      return;
    case Opcode::Store:
      out += "store ";
      printTypedValue(out, n, i->operand(0));
      out += ", ";
      printTypedValue(out, n, i->operand(1));
      return;
    case Opcode::GEP:
      out += "getelementptr ";
      out += typeText(i->srcElemType());
      out += ", ";
      printTypedValue(out, n, i->operand(0));
      out += ", ";
      printTypedValue(out, n, i->operand(1));
      return;
    case Opcode::BitCast:
      out += "bitcast ";
      printTypedValue(out, n, i->operand(0));
      out += " to ";
      out += typeText(i->type());
      return;
    // ── 比较（谓词 + 两个操作数；第二个不重复类型）─────────────────────
    case Opcode::ICmp:
    case Opcode::FCmp:
      out += opcodeName(i->op());
      out += ' ';
      out += (i->op() == Opcode::ICmp) ? ipredName(i->ipred()) : fpredName(i->fpred());
      out += ' ';
      printTypedValue(out, n, i->operand(0));
      out += ", ";
      out += nameOf(n, i->operand(1));
      return;
    // ── 转换（`<op> <ty> %a to <ty>`）──────────────────────────────────
    case Opcode::SIToFP:
    case Opcode::FPToSI:
    case Opcode::FPExt:
    case Opcode::SExt:
    case Opcode::ZExt:
    case Opcode::Trunc:
      out += opcodeName(i->op());
      out += ' ';
      printTypedValue(out, n, i->operand(0));
      out += " to ";
      out += typeText(i->type());
      return;
    // ── 调用（含两个内建）─────────────────────────────────────────────
    case Opcode::Call:
    case Opcode::LLVMMemCpy:
    case Opcode::LLVMMemSet:
      out += "call ";
      if (i->type() == nullptr || i->type()->isVoid()) {
        out += "void ";
      } else {
        out += typeText(i->type());
        out += ' ';
      }
      out += '@';
      out += i->callee();
      out += '(';
      for (size_t k = 0; k < nops; ++k) {
        if (k > 0) out += ", ";
        printTypedValue(out, n, i->operand(k));
      }
      out += ')';
      return;
    // ── φ（入值与前驱块**显式**配对）──────────────────────────────────
    case Opcode::Phi:
      out += "phi ";
      out += typeText(i->type());
      out += " [";
      for (size_t k = 0; k < nops; ++k) {
        if (k > 0) out += ", ";
        out += '(';
        out += nameOf(n, i->operand(k));
        out += " L";
        const BasicBlock* pb = i->succ(k);
        out += std::to_string(pb != nullptr ? pb->index() : 0);
        out += ')';
      }
      out += ']';
      return;
    case Opcode::Select:
      out += "select i1 ";
      out += nameOf(n, i->operand(0));
      out += ", ";
      printTypedValue(out, n, i->operand(1));
      out += ", ";
      printTypedValue(out, n, i->operand(2));
      return;
    // ── 一元 / 二元运算（`<op> <ty> %a[, %b]`）────────────────────────
    default:
      out += opcodeName(i->op());
      out += ' ';
      out += typeText(i->type());
      out += ' ';
      if (nops > 0) out += nameOf(n, i->operand(0));
      if (nops > 1) {
        out += ", ";
        out += nameOf(n, i->operand(1));
      }
      return;
  }
}

// 【后置】给函数内的值发号（参数 → 逐块逐指令 → 常量在首次引用时）。
void numberFunction(const Function& f, Numbering& n) {
  for (size_t i = 0; i < f.numParams(); ++i) nameFor(n, f.param(i));
  // ★ **φ 先发号**：φ 排在块首（后端契约 4），它们的定义行在 dump 里也
  //   出现在块内其它指令之前 ⇒ 编号必须与**打印顺序**一致。
  //   ⚠️ 少了这一步会怎样：读回器把 φ 延后建（它的入值要引用后面才出现的块），
  //   于是 φ 在"值的创建顺序"里排到了最后，编号与原文错位 ——
  //   症状是"dump 内容一样、只有编号不同"（`17_div.sy` 的 `%12` vs `%14`）。
  for (size_t bi = 0; bi < f.blockCount(); ++bi) {
    const BasicBlock* b = f.at(bi);
    for (size_t ii = 0; ii < b->size(); ++ii) {
      if (b->at(ii) != nullptr && b->at(ii)->op() == Opcode::Phi) nameFor(n, b->at(ii));
    }
  }
  for (size_t bi = 0; bi < f.blockCount(); ++bi) {
    const BasicBlock* b = f.at(bi);
    for (size_t ii = 0; ii < b->size(); ++ii) {
      const Instruction* inst = b->at(ii);
      if (inst->hasResult()) nameFor(n, inst);
      for (size_t k = 0; k < inst->numOperands(); ++k) {
        const Value* v = inst->operand(k);
        // 常量的定义在这里补号（**首次被引用时**，见文件头）
        if (v != nullptr && (v->isGlobalAddr() ||
                             (v->isInst() &&
                              static_cast<const Instruction*>(v)->isConstant()))) {
          nameFor(n, v);
        }
      }
    }
  }
}

void appendGlobal(std::string& out, const GlobalVariable& g) {
  out += '@';
  out += g.name();
  out += " = global ";
  out += typeText(g.objType());
  out += ' ';
  if (!g.hasData()) {
    out += "zeroinitializer";
  } else {
    out += '{';
    const GlobalVariable::Data& d = g.initData();
    for (size_t i = 0; i < d.size(); ++i) {
      if (i > 0) out += ',';
      out += ' ';
      out += std::to_string(d[i].first);
      out += '=';
      out += std::to_string(static_cast<int32_t>(d[i].second));
    }
    out += " }";
  }
  appendLine(out, g.loc().line);
  out += '\n';
}

void appendFunction(std::string& out, const Function& f) {
  if (f.isDeclaration()) return;   // 本关不产出不带体的定义/声明
  Numbering n;
  numberFunction(f, n);

  out += "define ";
  out += typeText(f.retType());
  out += " @";
  out += f.name();
  out += '(';
  for (size_t i = 0; i < f.numParams(); ++i) {
    if (i > 0) out += ", ";
    out += typeText(f.paramType(i));
    out += ' ';
    out += nameOf(n, f.param(i));
  }
  out += ") {\n";

  // 常量定义行：插在第一条指令之前、按编号升序（见文件头）。
  if (!n.consts.empty()) {
    std::vector<const Instruction*> cs = n.consts;
    for (const Instruction* c : cs) {
      out.append(kIndent, ' ');
      out += nameOf(n, c);
      out += " = ";
      std::string body;
      printInst(body, n, c);
      out += body;
      appendLine(out, c->loc().line);
      out += '\n';
    }
  }

  // 全局地址的定义行（`%N = ptr[T] @g`）—— 与常量同处"函数头部定义区"。
  //   为什么必须有它：平面层的操作数一律是 `%N`（设计文档 §4.1），
  //   而全局变量的地址**必须能被引用**（`g[0] = 1` 的第一步就是取 `g` 的地址）。
  //   不物化它就只能打 `%?`（实测：那样 `--from-flat` 读回的 IR 是坏的）。
  for (const GlobalAddr* ga : n.globals) {
    out.append(kIndent, ' ');
    out += nameOf(n, ga);
    out += " = ";
    out += typeText(ga->type());
    out += " @";
    out += ga->global() != nullptr ? ga->global()->name() : "?";
    out += '\n';
  }
  for (size_t bi = 0; bi < f.blockCount(); ++bi) {
    const BasicBlock* b = f.at(bi);
    out += 'L';
    out += std::to_string(b->index());
    out += ":\n";
    for (size_t ii = 0; ii < b->size(); ++ii) {
      const Instruction* inst = b->at(ii);
      out.append(kIndent, ' ');
      if (inst->hasResult()) {
        out += nameOf(n, inst);
        out += " = ";
      }
      std::string body;
      printInst(body, n, inst);
      out += body;
      appendLine(out, inst->loc().line);
      out += '\n';
    }
  }
  out += "}\n";
}

}  // namespace

std::string dumpModule(const Module& m) {
  std::string out;
  out.reserve(1 << 16);
  for (size_t i = 0; i < m.numGlobals(); ++i) {
    const GlobalVariable* g = m.global(i);
    if (g != nullptr) appendGlobal(out, *g);
  }
  for (size_t i = 0; i < m.numFunctions(); ++i) {
    const Function* f = m.function(i);
    if (f != nullptr) appendFunction(out, *f);
  }
  return out;
}

}  // namespace flat
}  // namespace sysy
