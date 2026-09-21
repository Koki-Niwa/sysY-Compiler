// ============================================================================
// StructuredDump.cpp —— 转储格式的**唯一实现**（规则见 StructuredDump.h 文件头）
//
// ── 遍历模型：先把"输出动作"编成一个**扁平脚本**，再顺序执行 ──────────────
//   结构化 IR 是树，而文本是行序列。最容易写错的地方是"括号的缩进与顺序"
//   （实测第一版就在多 Region 与闭合括号上错了两次：闭合括号缩进错位、
//    `{` 跑到别的 Op 后面）。所以这里分两步，两步都是**显式栈**（不用递归）：
//     ① `appendScript()`：先序展开整棵树，把每个动作编成一个 `Frame`
//        （打印某行 / 缩进 +1 / 缩进 -1 / 打印一个括号行）；
//     ② 顺序执行动作序列，`depth` 只被显式动作改。
//   于是"某个括号在多少缩进、在哪一行"是**数据**，不是"执行到那里时的状态"，
//   这类 bug 就不可能写出来。
//
// ── 为什么不用"内联常量"（一个**被实测推翻**的格式决定）──────────────────
//   第一版把 `IntOp`/`FloatOp` 在操作数位置打印成字面量（`i32 5`），理由是想让
//   dump 短一些。但那样一来**常量没有定义行**，读回器只能"现场造一个常量 Op"，
//   于是往返后同一段代码会多出重复的常量 Op（实测：往返后 6 行变 9 行，
//   `%main.N` 全部串位）—— 而这正是轨 A 要抓的东西。
//   ⇒ 最终采用**统一规则**：*每一个值都打印它的结果名*，常量也不例外。
//     好处有三个：① 往返必然同构；② "0..N 结果"在文本里处处可见；
//     ③ 读回器不需要"猜"哪条指令该被造出来。
// ============================================================================
#include "structured/StructuredDump.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace sysy {
namespace sir {
namespace {

constexpr int kIndent = 2;   // 每层缩进 2 空格（prompt §六）

// ── 浮点：**十六进制浮点**（`%a`）─────────────────────────────────────────
//   为什么不用十进制：`0.1f` 没有精确的十进制短表示，十进制往返会改变位模式
//   （轨 A 要求 dump→读回→dump **逐字节相同**，位模式变了就不成立）。
//   `%a` 是精确的、可被 strtof 无损读回的表示（与 S04 的 initplan 一致）。
//   ⚠️ 特殊值显式处理：NaN 的**载荷**无法用 %a 表达，我们只需保证"是 NaN"。
void appendHexFloat(std::string& out, uint32_t bits) {
  float f = 0.0f;
  static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
  std::memcpy(&f, &bits, sizeof(f));
  if (f != f) { out += "nan"; return; }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%a", static_cast<double>(f));
  out += buf;
}

// ── 引号字符串（函数名 / 变量名 / 源文件基名）────────────────────────────
//   SysY 标识符不含引号或反斜杠，所以"只处理这两个字符"是完备的；仍然写全，
//   因为读回器必须能吃掉任意畸形输入而不崩（C5：不用 assert 代替错误处理）。
void appendQuoted(std::string& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
}

// 结果名：`%<函数名>.<序号>`（函数内全局递增；多结果 Op 的每个结果各有一个名字）。
std::string resultName(const std::string& func, uint32_t n) {
  std::string s = "%";
  s += func;
  s += '.';
  s += std::to_string(n);
  return s;
}

// 预扫得到的两张表（Arena 保证 `const Result*` 稳定）。
struct NameTable {
  // 结果指针 → 名字（`%main.7`）。
  std::unordered_map<const Result*, std::string> names;
  // （S05 的最终决定：**常量不内联**，见文件头"为什么不用内联常量"）
};

// ① 先序展开：给每个结果分配名字，并记下常量结果的内联文本。
void scanNames(const Op* module, NameTable& t) {
  std::string curFunc;
  uint32_t counter = 0;
  std::vector<const Op*> stack;
  stack.push_back(module);
  while (!stack.empty()) {
    const Op* op = stack.back();
    stack.pop_back();
    if (op == nullptr) continue;
    if (op->kind == OpKind::Func) {
      curFunc = op->strAttr(0);
      counter = 0;
    }
    for (size_t i = 0; i < op->numResults(); ++i) {
      const Result* r = op->result(i);
      t.names[r] = resultName(curFunc, counter++);
    }
    for (size_t i = op->numRegions(); i-- > 0;) {
      const Region* reg = op->region(i);
      if (reg == nullptr) continue;
      for (size_t j = reg->size(); j-- > 0;) stack.push_back(reg->at(j));
    }
  }
}

// ── 输出脚本 ─────────────────────────────────────────────────────────────
//   一个动作只做一件事：打印"Op 行"、"`{` 行"、"`}` 行"，或改缩进。
struct Frame {
  enum class K : uint8_t { Op, Brace, IndentUp, IndentDown } k = K::Op;
  const Op* op = nullptr;   // Op 用
  bool close = false;       // Brace 用：true = `}`，false = `{`
};

// ② 一次先序展开整棵树 ⇒ 动作序列（**显式栈**）。
void appendScript(const Op* module, std::vector<Frame>& out) {
  struct Item {
    enum class K : uint8_t { Op, BraceOpen, BraceClose, IndentUp, IndentDown } k;
    const Op* op;
  };
  std::vector<Item> st;
  st.push_back(Item{Item::K::Op, module});
  while (!st.empty()) {
    const Item it = st.back();
    st.pop_back();
    switch (it.k) {
      case Item::K::BraceOpen:
        out.push_back(Frame{Frame::K::Brace, nullptr, false});
        break;
      case Item::K::BraceClose:
        out.push_back(Frame{Frame::K::Brace, nullptr, true});
        break;
      case Item::K::IndentUp:
        out.push_back(Frame{Frame::K::IndentUp, nullptr, false});
        break;
      case Item::K::IndentDown:
        out.push_back(Frame{Frame::K::IndentDown, nullptr, false});
        break;
      case Item::K::Op: {
        out.push_back(Frame{Frame::K::Op, it.op, false});
        if (it.op == nullptr || it.op->numRegions() == 0) break;
        // 反序压栈 ⇒ 弹出即先序。
        //   约定：**第一个 Region 的 `{` 跟在 Op 行尾**（由 Op 动作补），
        //   其余 Region 的 `{` 各自占一行（BraceOpen）。
        for (size_t i = it.op->numRegions(); i-- > 0;) {
          const Region* r = it.op->region(i);
          st.push_back(Item{Item::K::BraceClose, nullptr});
          st.push_back(Item{Item::K::IndentDown, nullptr});
          if (r != nullptr) {
            const std::vector<Op*>& ops = r->ops();
            for (size_t j = ops.size(); j-- > 0;) {
              st.push_back(Item{Item::K::Op, ops[j]});
            }
          }
          st.push_back(Item{Item::K::IndentUp, nullptr});
          if (i > 0) st.push_back(Item{Item::K::BraceOpen, nullptr});
        }
        break;
      }
    }
  }
}

}  // namespace

std::string dumpModule(const Op* module) {
  std::string out;
  if (module == nullptr) return out;
  out.reserve(4096);

  NameTable names;
  scanNames(module, names);

  std::vector<Frame> script;
  appendScript(module, script);

  int depth = 0;
  for (const Frame& f : script) {
    switch (f.k) {
      case Frame::K::IndentUp:
        ++depth;
        continue;
      case Frame::K::IndentDown:
        --depth;
        continue;
      case Frame::K::Brace:
        out.append(static_cast<size_t>(depth * kIndent), ' ');
        out += f.close ? "}\n" : "{\n";
        continue;
      case Frame::K::Op:
        break;
    }
    const Op* op = f.op;
    if (op == nullptr) { out += '\n'; continue; }   // 畸形 IR：空 Op 槽

    out.append(static_cast<size_t>(depth * kIndent), ' ');
    out += '(';
    out += opKindName(op->kind);

    // ── 一行之内的顺序（**必须与 StructuredReader 逐字对应**）────────────
    //   ① `Call`/`GetGlobal` 的名字 ② 结果（0..N）③ 头部属性 ④ 其余属性
    //   ⑤ 操作数 ⑥ `@line`
    //   ★ 为什么 `Call` 的名字在结果**之前**：被调函数决定"有没有结果"
    //     （void 函数没有结果），所以读回器必须先看到名字才能判定。
    //     其余 Op 的结果个数由 OpKind 唯一决定，名字放在结果之后更自然。
    if (op->kind == OpKind::Call || op->kind == OpKind::GetGlobal) {
      out += ' ';
      appendQuoted(out, op->strAttr(0));
    }
    for (size_t i = 0; i < op->numResults(); ++i) {
      out += ' ';
      const auto it = names.names.find(op->result(i));
      out += (it == names.names.end()) ? std::string("%?") : it->second;
    }
    // ── 头部属性（按 OpKind）────────────────────────────────────────────
    size_t skip = 0;
    if (op->kind == OpKind::Call) {
      skip = 1;   // 名字已在上面打过
    } else if (op->kind == OpKind::Module) {
      out += ' ';
      appendQuoted(out, op->strAttr(0));
      skip = 1;
    } else if (op->kind == OpKind::Func) {
      // 函数头带签名（prompt §六：S06 要按它分配）
      out += ' ';
      appendQuoted(out, op->strAttr(0));
      out += " :ret ";
      appendTypeText(out, op->typeAttr(1));
      out += " :param [";
      for (size_t i = 2; i < op->attrs().size(); ++i) {
        if (i > 2) out += ", ";
        appendTypeText(out, op->attrs()[i].ty);
      }
      out += ']';
      skip = op->attrs().size();
    } else if (op->kind == OpKind::GetGlobal) {
      out += " :type ";
      appendTypeText(out, op->typeAttr(1));
      skip = 2;
    } else if (op->kind == OpKind::GlobalVar) {
      out += ' ';
      appendQuoted(out, op->strAttr(0));
      out += " :type ";
      appendTypeText(out, op->typeAttr(1));
      out += " :init ";
      appendQuoted(out, op->strAttr(2));
      skip = 3;
    }

    // 剩余属性（`Int`/`Float` 的"值"就是它唯一的属性，所以这里会打印它）
    for (size_t i = skip; i < op->attrs().size(); ++i) {
      const Attr& a = op->attrs()[i];
      switch (a.kind) {
        case Attr::Kind::Str:   out += ' '; appendQuoted(out, a.s); break;
        case Attr::Kind::Type:  out += ' '; appendTypeText(out, a.ty); break;
        case Attr::Kind::Int:   out += ' '; out += std::to_string(a.i); break;
        case Attr::Kind::Float: out += ' '; appendHexFloat(out, a.fbits); break;
        case Attr::Kind::Data: {
          out += " {";
          for (size_t k = 0; k < a.data.size(); ++k) {
            if (k > 0) out += ", ";
            out += std::to_string(a.data[k].first);
            out += '=';
            out += std::to_string(static_cast<int32_t>(a.data[k].second));
          }
          out += '}';
          break;
        }
      }
    }
    // 操作数：一律引用结果名（`%f.N`）。
    //   ★ 常量也是 Op，也有名字 —— **统一规则**让"往返同构"成为结构性质：
    //     读回器不需要"猜"哪个操作数该现场造一个常量 Op。
    //   （曾试过"把常量内联成 `i32 5` 省行数"，两次都破坏了往返：一次是常量
    //     没有定义行导致读回器造重复 Op，一次是 `f32 0x1p+31` 与裸整数属性
    //     在文本上不可区分。**格式的规则越少越安全**。）
    for (size_t i = 0; i < op->numOperands(); ++i) {
      const Value v = op->operand(i);
      out += ' ';
      if (v == nullptr) { out += "%?"; continue; }
      const auto it = names.names.find(v);
      out += (it == names.names.end()) ? std::string("%?") : it->second;
    }
    // D11：SourceLoc 一路带到 IR（`starttime` 依赖它）。纯函数的一部分。
    if (op->loc.line != 0) {
      out += " @line ";
      out += std::to_string(op->loc.line);
    }
    out += ')';
    // 第一个 Region 的 `{` 跟在 Op 行尾（其余 Region 的 `{` 是独立的 Brace 动作）
    if (op->numRegions() > 0) out += " {";
    out += '\n';
  }
  return out;
}

}  // namespace sir
}  // namespace sysy
