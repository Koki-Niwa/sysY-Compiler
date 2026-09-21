// ============================================================================
// StructuredReaderLex.h —— 读回器的**词法层**（内部头；只有一份实现）
//
//   为什么单独一个文件：`StructuredReader.cpp` 的两趟解析逻辑本身已经很长，
//   而"一行 → 记号序列"是**完全独立**的一层（不依赖任何容器类型）。
//   拆开让两边都能一眼读完（§C4 单一职责）。
//
// ── `.` 必须同时是"数字的一部分"与"标识符的一部分" ───────────────────────
//   `0x1p+31`（十六进制浮点的指数）与 `llvm.memcpy` / `%main.7`（内建名、
//   结果名）都需要 `.`。所以词法按**上下文**分派：`.` 前面是数字 ⇒ 归数字；
//   否则 ⇒ 归标识符。这是实测踩出来的（少了任何一边都会报"无法识别的字符"）。
// ============================================================================
#ifndef SYSY_STRUCTURED_STRUCTUREDREADERLEX_H
#define SYSY_STRUCTURED_STRUCTUREDREADERLEX_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sysy {
namespace sir {

// ============================================================================
// 0. 词法：一行 → 记号序列
// ============================================================================
enum class TK : uint8_t {
  LParen, RParen, LBracket, RBracket, LBrace, RBrace,
  Comma, Equals, Percent, AtLine, Colon, Str, Num, Ident
};
struct Tok {
  TK k = TK::Ident;
  std::string s;   // Str / Ident / Num 的文本（其它为空）
};
struct Line {
  std::vector<Tok> toks;
  bool openBrace = false;        // 行尾是 `{`（Region 开始）
  bool closeBraceFirst = false;  // 行首是 `}`（Region 结束）
};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isNumStart(const std::string& s, size_t i) {
  if (isDigit(s[i])) return true;
  return (s[i] == '-' || s[i] == '+') && i + 1 < s.size() && isDigit(s[i + 1]);
}
bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '<' ||
         c == '>' || c == '?';
}
bool isIdentChar(char c) {
  return isIdentStart(c) || isDigit(c) || c == '.';
}

// 【后置】把一行切成记号；遇到无法识别的字符返回 false（畸形输入要报错，
//         不要"猜个近似的继续"）。
bool lexLine(const std::string& s, Line& out) {
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
    if (c == '(') { out.toks.push_back({TK::LParen, "("}); ++i; continue; }
    if (c == ')') { out.toks.push_back({TK::RParen, ")"}); ++i; continue; }
    if (c == '[') { out.toks.push_back({TK::LBracket, "["}); ++i; continue; }
    if (c == ']') { out.toks.push_back({TK::RBracket, "]"}); ++i; continue; }
    if (c == ',') { out.toks.push_back({TK::Comma, ","}); ++i; continue; }
    if (c == '=') { out.toks.push_back({TK::Equals, "="}); ++i; continue; }
    if (c == '%') { out.toks.push_back({TK::Percent, "%"}); ++i; continue; }
    if (c == ':') { out.toks.push_back({TK::Colon, ":"}); ++i; continue; }
    if (c == '{') {
      // `{` 在**行尾** ⇒ Region 开括号；否则是 `:data {...}` 的表（同行内）。
      size_t j = i + 1;
      while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) ++j;
      if (j >= s.size()) { out.openBrace = true; ++i; continue; }
      out.toks.push_back({TK::LBrace, "{"});
      ++i;
      continue;
    }
    if (c == '}') {
      if (out.toks.empty()) { out.closeBraceFirst = true; ++i; continue; }
      out.toks.push_back({TK::RBrace, "}"});
      ++i;
      continue;
    }
    if (c == '@') {
      if (s.compare(i, 5, "@line") != 0) return false;
      out.toks.push_back({TK::AtLine, "@line"});
      i += 5;
      continue;
    }
    if (c == '"') {
      std::string v;
      ++i;
      bool closed = false;
      while (i < s.size()) {
        if (s[i] == '\\' && i + 1 < s.size()) { v += s[i + 1]; i += 2; continue; }
        if (s[i] == '"') { ++i; closed = true; break; }
        v += s[i++];
      }
      if (!closed) return false;
      out.toks.push_back({TK::Str, v});
      continue;
    }
    if (isNumStart(s, i)) {
      // 整数部分（十进制 / `0x` 十六进制）→ 小数 → 指数。
      //   ★ 指数只在**后面真的跟着数字**时才吃掉 `+`/`-`：否则 `0x1p+31@line 3`
      //     的 `+` 会被吞（实测踩过）。
      const size_t b = i;
      if (s[i] == '-' || s[i] == '+') ++i;
      const bool hex = (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X'));
      if (hex) i += 2;
      while (i < s.size()) {
        const char d = s[i];
        if (isDigit(d)) { ++i; continue; }
        if (hex && ((d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F'))) { ++i; continue; }
        break;
      }
      if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size()) {
          const char d = s[i];
          if (isDigit(d)) { ++i; continue; }
          if (hex && ((d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F'))) { ++i; continue; }
          break;
        }
      }
      const char e1 = hex ? 'p' : 'e';
      if (i < s.size() && (s[i] == e1 || s[i] == static_cast<char>(e1 - 32))) {
        size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
        if (j < s.size() && isDigit(s[j])) {
          i = j;
          while (i < s.size() && isDigit(s[i])) ++i;
        }
      }
      out.toks.push_back({TK::Num, s.substr(b, i - b)});
      continue;
    }
    if (c == '.') {   // `%.0`（模块级结果名）
      const size_t b = i;
      while (i < s.size() && (isIdentChar(s[i]) || isDigit(s[i]))) ++i;
      out.toks.push_back({TK::Ident, s.substr(b, i - b)});
      continue;
    }
    if (isIdentStart(c)) {
      const size_t b = i;
      while (i < s.size() && isIdentChar(s[i])) ++i;
      out.toks.push_back({TK::Ident, s.substr(b, i - b)});
      continue;
    }
    return false;
  }
  return true;
}

bool parseI32(const std::string& s, int32_t& out) {
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || errno == ERANGE) return false;
  out = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
  return true;
}

bool parseF32Bits(const std::string& s, uint32_t& bits) {
  if (s == "nan" || s == "-nan" || s == "+nan") { bits = 0x7fc00000u; return true; }
  errno = 0;
  char* end = nullptr;
  const float f = std::strtof(s.c_str(), &end);
  if (end == nullptr || *end != '\0' || errno == ERANGE) return false;
  std::memcpy(&bits, &f, sizeof(bits));
  return true;
}

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_STRUCTUREDREADERLEX_H
