// ============================================================================
// ir/FlatLex.h —— 平面 IR 文本的**记号扫描器**（读回器专用，`ir/` 内部）
//
//   为什么单独一个头：`FlatReader.cpp` 超过 §C4 的 600 行上限，而"把一行文本
//   切成记号"与"读回器的状态机"是两件事 —— 切分线就在这里。
//   本头**只被 `FlatReader.cpp` 包含**（`flat::detail` 是实现细节）。
//
// ── 记号规则（与 `FlatDump.cpp` 的打印规则逐字对应）──────────────────────
//   * 类型文本**整段成号**：`i32` / `ptr[i32]` / `[3 x [2 x i32]]`。
//     ⚠️ 判据是"标识符后面紧跟 `[`"（`ptr[…`）或直接以 `[` 开头；两者都要
//     **吃到括号配平**。否则括号会被切成独立记号，`parseTypeText` 只看到 `p`
//     —— 这是"`ptr[i32]` 读不回来"的直接原因（实测）。
//   * 标点 `(` `)` `{` `}` `,` `=` 各自成号。
//   * 其余记号读到空白/标点/`[`/`]` 为止（`%3` / `@line` / `0x1.8p+1` …）。
// ============================================================================
#ifndef SYSY_IR_FLATLEX_H
#define SYSY_IR_FLATLEX_H

#include <cstdint>
#include <cstdlib>

#include "support/SourceLoc.h"
#include <cstdlib>

#include "support/SourceLoc.h"
#include <string>
#include <vector>

namespace sysy {
namespace flat {
namespace detail {

// 【后置】把 `line` 切成记号并追加到 `out`（畸形输入不抛异常、不死循环）。
inline void tokenizeLine(const std::string& line, std::vector<std::string>& out) {
  size_t i = 0;
  const size_t n = line.size();
  auto isIdentChar = [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
  };
  while (i < n) {
    while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= n) break;
    const char c = line[i];
    if (c == '(' || c == ')' || c == '{' || c == '}' || c == ',' || c == '=') {
      out.emplace_back(1, c);
      ++i;
      continue;
    }
    // ★ 只有两种情况算"类型记号"，必须整段吃到配平：
    //   ① `ptr[` —— 标识符后面紧跟 `[`；
    //   ② `[<数字>` —— **独立的数组类型**（`[3 x i32]`）。
    //   ⚠️ **裸 `[` 不能**无条件当成类型：φ 的入值列表也是 `[` 开头
    //   （`phi i32 [(%5 L1), (%13 L2)]`），整段吞掉会让 φ 解析失败
    //   （实测："phi 的入值必须是 `(%v LN)`"）。
    bool typeStart = false;
    {
      size_t j = i;
      while (j < n && isIdentChar(line[j])) ++j;
      if (j > i && j < n && line[j] == '[') {
        typeStart = true;                        // `ptr[` / `[N x T]` 之类的标识符前缀
      } else if (c == '[') {
        size_t q = i + 1;
        while (q < n && (line[q] == ' ' || line[q] == '\t')) ++q;
        typeStart = (q < n && line[q] >= '0' && line[q] <= '9');   // `[3 x i32]`
      }
    }
    if (typeStart) {
      // ★★ 括号配平循环（**这里曾经有一个"恒真"的退出条件**）★★
      //   `depth` 从 0 开始 ⇒ 进入循环后的第一次判断 `depth == 0` 就成立
      //   （第一个字符 `p` 既不是 `[` 也不是 `]`）⇒ 循环**只吃到首字符**。
      //   症状：`ptr[i32]` 被切成 `[p] [t] [r] [[i32]]`，`parseTypeText` 只看到
      //   `p` ⇒ 读回报"常量类型无法解析：p" / "getelementptr 的操作数无法解析"
      //   ⇒ **`--from-flat` 往返与轨 D 的逐字节比对全都失败**（166 个文件）。
      //   ⇒ 修法：用一个"还没进到括号里"的哨兵（`depth < 0` 表示尚未见到 `[`），
      //     见到 `[` 之后才开始数，回到 0 才算这一整段类型结束。
      int depth = -1;
      std::string tstr;
      while (i < n) {
        if (line[i] == '[') {
          if (depth < 0) depth = 0;
          ++depth;
        }
        if (line[i] == ']' && depth > 0) --depth;
        tstr += line[i];
        ++i;
        if (depth == 0) break;
      }
      out.push_back(tstr);
      continue;
    }
    const size_t start = i;
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '(' &&
           line[i] != ')' && line[i] != '{' && line[i] != '}' && line[i] != ',' &&
           line[i] != '=' && line[i] != '[' && line[i] != ']') {
      ++i;
    }
    if (i == start) { ++i; continue; }   // 保险：绝不空转
    out.push_back(line.substr(start, i - start));
  }
}

// 【后置】十进制 / 有符号整数解析；失败返回 false（不抛异常）。
inline bool parseI64(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<int64_t>(v);
  return true;
}

// 【后置】无符号 32 位解析（`%12` / `L3` 的数值部分）。
inline bool parseU32(const std::string& s, uint32_t& out) {
  int64_t v = 0;
  if (!parseI64(s, v)) return false;
  if (v < 0) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// 【后置】`%12` → 12。
inline bool parseValueRef(const std::string& s, uint32_t& out) {
  if (s.size() < 2 || s[0] != '%') return false;
  return parseU32(s.substr(1), out);
}

// 【后置】`L3` / `L3:` → 3。
inline bool parseBlockRef(const std::string& s, uint32_t& out) {
  if (s.size() < 2 || s[0] != 'L') return false;
  return parseU32(s.substr(1), out);
}

// 【后置】从记号里找 `@line N`（找不到 → 空 SourceLoc）。
//   纯函数 ⇒ 放在工具层，读回器的多个解析点共用。
inline SourceLoc lineFromTokens(const std::vector<std::string>& tok, size_t from) {
  for (size_t i = from; i + 1 < tok.size(); ++i) {
    if (tok[i] == "@line") {
      int64_t v = 0;
      if (parseI64(tok[i + 1], v)) return SourceLoc(static_cast<uint32_t>(v), 0);
    }
  }
  return SourceLoc();
}

}  // namespace detail
}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_FLATLEX_H
