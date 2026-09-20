// ============================================================================
// Lexer.cpp —— SysY 词法分析器实现（S01）
//
// 扫描策略（单遍、无回溯、O(n)）：
//
//   next()
//     ├─ skipTrivia()            空白 / // 行注释 / /* 块注释
//     ├─ atEnd()      → EndOfFile
//     ├─ 标识符/关键字 → scanIdentOrKeyword()
//     ├─ 数字          → scanNumber()
//     ├─ 'operators'  → 最长匹配（两字符优先）
//     └─ 其他          → makeInvalid()（报 error，继续扫描）
//
// 【为什么行列号用 src_.locOf(pos) 算】SourceFile 已经维护了行首索引
// （upper_bound 查表），无需在 Lexer 里重复一份行号状态机 —— 少一份状态就少一类
// 不一致的 bug（比如 CRLF 规范化后行号错位）。
// ============================================================================
#include "frontend/Lexer.h"

#include <string>

#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace sysy {

namespace {

// 字符分类（只按字节判断：SysY 源码的 token 全是 ASCII；
// 非 ASCII 字节只可能出现在注释里，而注释由 skipTrivia 整段跳过）
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isHexDigit(char c) {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }
// 空白：与 C 一致（空格、\t、\n、\v、\f）。
// 【不含 '\r'】—— SourceFile 已把 \r\n 与孤立 \r 都规范化成 '\n'，
// 残留的 '\r' 只可能来自 NUL 之类的异常输入，按非法字符报出来更诚实。
inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f';
}

// 关键字表：**只有规范里的这 10 个**。
// tensor / for / do / while(在) / struct 等都不在这里 —— 它们是普通标识符。
struct KeywordEntry {
  const char* text;
  TokKind kind;
};
const KeywordEntry kKeywords[] = {
    {"const", TokKind::KwConst},       {"int", TokKind::KwInt},
    {"float", TokKind::KwFloat},       {"void", TokKind::KwVoid},
    {"if", TokKind::KwIf},             {"else", TokKind::KwElse},
    {"while", TokKind::KwWhile},       {"break", TokKind::KwBreak},
    {"continue", TokKind::KwContinue}, {"return", TokKind::KwReturn},
};

TokKind lookupKeyword(const char* s, size_t n) {
  for (const KeywordEntry& e : kKeywords) {
    // 先比长度（strncmp 需要 C 字符串；这里用固定长度比较，避免越界读）
    size_t i = 0;
    while (i < n && e.text[i] != '\0' && e.text[i] == s[i]) ++i;
    if (i == n && e.text[i] == '\0') return e.kind;
  }
  return TokKind::Ident;
}

}  // namespace

// ============================================================================
// Token 类别判定与名字
// ============================================================================
bool isKeyword(TokKind k) {
  switch (k) {
    case TokKind::KwConst:
    case TokKind::KwInt:
    case TokKind::KwFloat:
    case TokKind::KwVoid:
    case TokKind::KwIf:
    case TokKind::KwElse:
    case TokKind::KwWhile:
    case TokKind::KwBreak:
    case TokKind::KwContinue:
    case TokKind::KwReturn:
      return true;
    default:
      return false;
  }
}

bool isLiteral(TokKind k) {
  return k == TokKind::IntLit || k == TokKind::FloatLit;
}

const char* tokKindName(TokKind k) {
  switch (k) {
    case TokKind::EndOfFile:  return "end of file";
    case TokKind::Ident:      return "identifier";
    case TokKind::IntLit:     return "integer literal";
    case TokKind::FloatLit:   return "floating literal";
    case TokKind::KwConst:    return "keyword 'const'";
    case TokKind::KwInt:      return "keyword 'int'";
    case TokKind::KwFloat:    return "keyword 'float'";
    case TokKind::KwVoid:     return "keyword 'void'";
    case TokKind::KwIf:       return "keyword 'if'";
    case TokKind::KwElse:     return "keyword 'else'";
    case TokKind::KwWhile:    return "keyword 'while'";
    case TokKind::KwBreak:    return "keyword 'break'";
    case TokKind::KwContinue: return "keyword 'continue'";
    case TokKind::KwReturn:   return "keyword 'return'";
    case TokKind::Plus:       return "'+'";
    case TokKind::Minus:      return "'-'";
    case TokKind::Star:       return "'*'";
    case TokKind::Slash:      return "'/'";
    case TokKind::Percent:    return "'%'";
    case TokKind::Less:       return "'<'";
    case TokKind::Greater:    return "'>'";
    case TokKind::LessEq:     return "'<='";
    case TokKind::GreaterEq:  return "'>='";
    case TokKind::EqEq:       return "'=='";
    case TokKind::NotEq:      return "'!='";
    case TokKind::Assign:     return "'='";
    case TokKind::Not:        return "'!'";
    case TokKind::AmpAmp:     return "'&&'";
    case TokKind::PipePipe:   return "'||'";
    case TokKind::LParen:     return "'('";
    case TokKind::RParen:     return "')'";
    case TokKind::LBrace:     return "'{'";
    case TokKind::RBrace:     return "'}'";
    case TokKind::LBracket:   return "'['";
    case TokKind::RBracket:   return "']'";
    case TokKind::Comma:      return "','";
    case TokKind::Semicolon:  return "';'";
    case TokKind::Invalid:    return "invalid character";
  }
  return "unknown token";
}

// --emit=tokens 的第 3 列。⚠️ 改这里等于改 dump 格式（对外契约）。
const char* tokKindDumpName(TokKind k) {
  switch (k) {
    case TokKind::EndOfFile:  return "EOF";
    case TokKind::Ident:      return "ident";
    case TokKind::IntLit:     return "intlit";
    case TokKind::FloatLit:   return "floatlit";
    case TokKind::KwConst:    return "kw_const";
    case TokKind::KwInt:      return "kw_int";
    case TokKind::KwFloat:    return "kw_float";
    case TokKind::KwVoid:     return "kw_void";
    case TokKind::KwIf:       return "kw_if";
    case TokKind::KwElse:     return "kw_else";
    case TokKind::KwWhile:    return "kw_while";
    case TokKind::KwBreak:    return "kw_break";
    case TokKind::KwContinue: return "kw_continue";
    case TokKind::KwReturn:   return "kw_return";
    case TokKind::Plus:       return "plus";
    case TokKind::Minus:      return "minus";
    case TokKind::Star:       return "star";
    case TokKind::Slash:      return "slash";
    case TokKind::Percent:    return "percent";
    case TokKind::Less:       return "less";
    case TokKind::Greater:    return "greater";
    case TokKind::LessEq:     return "lesseq";
    case TokKind::GreaterEq:  return "greatereq";
    case TokKind::EqEq:       return "eqeq";
    case TokKind::NotEq:      return "noteq";
    case TokKind::Assign:     return "assign";
    case TokKind::Not:        return "not";
    case TokKind::AmpAmp:     return "ampamp";
    case TokKind::PipePipe:   return "pipepipe";
    case TokKind::LParen:     return "lparen";
    case TokKind::RParen:     return "rparen";
    case TokKind::LBrace:     return "lbrace";
    case TokKind::RBrace:     return "rbrace";
    case TokKind::LBracket:   return "lbracket";
    case TokKind::RBracket:   return "rbracket";
    case TokKind::Comma:      return "comma";
    case TokKind::Semicolon:  return "semicolon";
    case TokKind::Invalid:    return "invalid";
  }
  return "unknown";
}

std::string_view tokenText(const Token& tok, const SourceFile& src) {
  if (tok.kind == TokKind::EndOfFile) return std::string_view();
  const std::string& t = src.text();
  if (tok.length == 0) {
    // 理论上不会发生（Lexer 保证 length >= 1）；防御性返回空 view 而不是越界读
    return std::string_view();
  }
  const size_t off = src.offsetOf(tok.loc);
  if (off >= t.size()) return std::string_view();
  const size_t avail = t.size() - off;
  return std::string_view(t.data() + off, tok.length < avail ? tok.length : avail);
}

// ============================================================================
// Lexer
// ============================================================================
Lexer::Lexer(const SourceFile& src, DiagnosticEngine& diag)
    : src_(src), diag_(diag), text_(src.text()) {}

size_t Lexer::errorCount() const { return errors_; }

Token Lexer::makeToken(TokKind kind, size_t start, size_t offset) {
  Token t;
  t.kind = kind;
  t.loc = src_.locOf(start);
  t.length = static_cast<uint32_t>(offset - start);
  if (kind == TokKind::Ident || isLiteral(kind)) {
    t.text = std::string_view(text_.data() + start, offset - start);
  }
  pos_ = offset;
  return t;
}

Token Lexer::makeInvalid(size_t start, const char* what) {
  ++errors_;
  const SourceLoc loc = src_.locOf(start);
  char shown = text_[start];
  std::string printable;
  if (shown >= 32 && shown < 127) {
    printable = std::string(1, shown);
  } else {
    printable = "\\x" + std::string(1, "0123456789abcdef"[(shown >> 4) & 0xF]) +
                std::string(1, "0123456789abcdef"[shown & 0xF]);
  }
  diag_.report(DiagLevel::Error, loc,
               std::string(what) + " '" + printable + "' (0x" +
                   std::string(1, "0123456789abcdef"[(shown >> 4) & 0xF]) +
                   std::string(1, "0123456789abcdef"[shown & 0xF]) + ")");
  // 长度恒为 1 → 保证 pos_ 一定前进（否则非法字符会导致死循环）
  return makeToken(TokKind::Invalid, start, start + 1);
}

// —— 注释与空白 ——
void Lexer::skipLineComment() {
  // pos_ 指向第一个 '/'；"//" 之后直到行尾（'\n' 本身不属于注释）
  pos_ += 2;
  while (!atEnd() && text_[pos_] != '\n') ++pos_;
}

void Lexer::skipBlockComment(SourceLoc startLoc) {
  // pos_ 指向第一个 '/'；跳过 "/*"，找第一个 "*/"
  pos_ += 2;
  while (!atEnd()) {
    if (text_[pos_] == '*' && peek(1) == '/') {
      pos_ += 2;
      return;
    }
    ++pos_;
  }
  // 未闭合：报到注释起始位置（比报到 EOF 更有用）
  ++errors_;
  diag_.report(DiagLevel::Error, startLoc, "unterminated block comment");
}

void Lexer::skipTrivia() {
  while (!atEnd()) {
    const char c = text_[pos_];
    if (isSpace(c)) {
      ++pos_;
    } else if (c == '/' && peek(1) == '/') {
      skipLineComment();
    } else if (c == '/' && peek(1) == '*') {
      const SourceLoc startLoc = src_.locOf(pos_);
      skipBlockComment(startLoc);
    } else {
      return;
    }
  }
}

// —— 数字扫描 ——
size_t Lexer::scanDecimalDigits(size_t p) const {
  while (p < text_.size() && isDigit(text_[p])) ++p;
  return p;
}

size_t Lexer::scanHexDigits(size_t p) const {
  while (p < text_.size() && isHexDigit(text_[p])) ++p;
  return p;
}

// 指数部分：首字符属于 markers（十进制 "eE"、十六进制 "pP"），
// 后接可选符号与【至少一位】数字。不满足则一字节都不消费（最长匹配失败即回退到 p）。
size_t Lexer::scanExponent(size_t p, const char* markers) const {
  if (p >= text_.size()) return p;
  const char c = text_[p];
  bool isMarker = false;
  for (const char* m = markers; *m != '\0'; ++m) {
    if (c == *m) { isMarker = true; break; }
  }
  if (!isMarker) return p;
  size_t q = p + 1;
  if (q < text_.size() && (text_[q] == '+' || text_[q] == '-')) ++q;
  if (q >= text_.size() || !isDigit(text_[q])) return p;   // "1e" / "1e+" → 不是浮点
  while (q < text_.size() && isDigit(text_[q])) ++q;
  return q;
}

// 扫描一个数字 token（整数或浮点）。返回 {结束 offset, 是否浮点}。
// 只推进、不回退：返回的 end 一定 > start（保证 next() 一定前进）。
//
// 覆盖的形态（全部实测出现在语料里，见 phases/S01-lexer.md §2.3）：
//   整数     0 · 123 · 070 · 0x1f · 0X1F
//   十进制浮 1.0 · .5 · 1. · 1e10 · 1.5e-3 · 1E+3 · 03.14159（八进制前缀+小数点→浮点）
//   十六进制浮 0x1.921fb6p+1 · 0x.AP-3 · 0x1p-3
Lexer::NumberScan Lexer::scanNumber(size_t start) {
  const size_t n = text_.size();
  size_t p = start;

  // ── 十六进制：0x / 0X ──────────────────────────────────────────────────
  if (text_[p] == '0' && p + 1 < n && (text_[p + 1] == 'x' || text_[p + 1] == 'X')) {
    p += 2;
    const size_t intStart = p;                 // 小数点【前】的十六进制数字
    p = scanHexDigits(p);
    const size_t intEnd = p;
    bool isFloat = false;
    if (p < n && text_[p] == '.' && p + 1 < n && isHexDigit(text_[p + 1])) {
      // 0x1.921fb6 / 0x.AP-3 —— C99 十六进制浮点（小数点后必须有数字）
      p = scanHexDigits(p + 1);
      isFloat = true;
    }
    if (p < n && (text_[p] == 'p' || text_[p] == 'P')) {
      const size_t q = scanExponent(p, "pP");
      if (q == p) {
        // `0x1p` / `0x1p+`：指数部分残缺 → 报错，token 到此为止（继续扫描）
        ++errors_;
        diag_.report(DiagLevel::Error, src_.locOf(p),
                     "exponent has no digits in hexadecimal floating literal");
      } else {
        p = q;
        isFloat = true;
      }
    }
    if (!isFloat && intEnd == intStart) {
      // `0x` 后面一个十六进制数字都没有（如 `0x;`）→ 报错，仍产出 IntLit 以便继续扫描
      ++errors_;
      diag_.report(DiagLevel::Error, src_.locOf(start),
                   "hexadecimal literal has no digits");
    }
    // `0x1.` 这种"有点但小数点后没数字"的情况：token 到 `0x1` 为止，
    // 后面的 '.' 由主循环单独处理（与 clang 的 C 词法一致）——不是浮点。
    return {p, isFloat};
  }

  // ── 十进制 / 八进制 / 十进制浮点 ───────────────────────────────────────
  //
  // ⚠️ 最容易被漏的一类：C99 允许小数点前后【任一侧为空】。
  //    `1.` / `2.`（数字 + 点，无小数部分）实测出现在 6 个文件里，
  //    朴素实现会把它切成 `1` + `.`（`.` → Invalid）→ 自校验失败。
  //    判据是"有小数点就是浮点"，**不要求**小数点后面有数字。
  const size_t intEnd = scanDecimalDigits(p);
  // ★ p 是"当前已消费到的位置"：没有小数点时它必须【推进到 intEnd】，
  //   否则下面的 scanExponent 会从 token 起点重新看，`1e-3` 会被切成 `1` `e` `-` `3`
  //   （实测 bug：check_lexer.py 的拼接不变式恰好对这种切分是"守恒"的，
  //     所以只有单元测试能抓住它 —— 这正是同时要两套验证的理由）。
  p = intEnd;
  bool isFloat = false;
  if (intEnd < n && text_[intEnd] == '.') {
    // 1.5 · 03.14159 · 2. 都是浮点；前导 0 在"有小数点"时无意义（C 的规则）
    p = scanDecimalDigits(intEnd + 1);   // 小数点后可以一个数字都没有
    isFloat = true;
  }
  const size_t q = scanExponent(p, "eE");
  if (q != p) {
    p = q;
    isFloat = true;
  }
  // IntLit：0 / 123 / 070（十进制/八进制由 S04 求值时按前导 0 区分，词法不求值）
  return {isFloat ? p : intEnd, isFloat};
}

// —— 主循环 ——
Token Lexer::next() {
  skipTrivia();

  if (atEnd()) {
    // EndOfFile：loc 指向文本末尾（CRLF 规范化后一定以 '\n' 结尾，列号即该行末尾之后）
    Token t;
    t.kind = TokKind::EndOfFile;
    t.loc = src_.locOf(text_.size());
    t.length = 0;
    return t;
  }

  const size_t start = pos_;
  const char c = text_[start];

  // ── 标识符 / 关键字 ────────────────────────────────────────────────────
  if (isIdentStart(c)) {
    size_t p = start + 1;
    while (p < text_.size() && isIdentCont(text_[p])) ++p;
    const TokKind k = lookupKeyword(text_.data() + start, p - start);
    return makeToken(k, start, p);
  }

  // ── 数字 ───────────────────────────────────────────────────────────────
  if (isDigit(c)) {
    const NumberScan ns = scanNumber(start);
    return makeToken(ns.isFloat ? TokKind::FloatLit : TokKind::IntLit, start, ns.end);
  }

  // ── `.5` 形式的浮点（小数点开头，后面必须紧跟数字）─────────────────────
  if (c == '.' && isDigit(peek(1))) {
    const NumberScan ns = scanNumber(start);
    return makeToken(TokKind::FloatLit, start, ns.end);
  }

  // ── 运算符与定界符（最长匹配：两字符优先）─────────────────────────────
  switch (c) {
    case '+': return makeToken(TokKind::Plus, start, start + 1);
    case '-': return makeToken(TokKind::Minus, start, start + 1);
    case '*': return makeToken(TokKind::Star, start, start + 1);
    case '/': return makeToken(TokKind::Slash, start, start + 1);   // // 与 /* 已在 skipTrivia 处理
    case '%': return makeToken(TokKind::Percent, start, start + 1);
    case '(': return makeToken(TokKind::LParen, start, start + 1);
    case ')': return makeToken(TokKind::RParen, start, start + 1);
    case '{': return makeToken(TokKind::LBrace, start, start + 1);
    case '}': return makeToken(TokKind::RBrace, start, start + 1);
    case '[': return makeToken(TokKind::LBracket, start, start + 1);
    case ']': return makeToken(TokKind::RBracket, start, start + 1);
    case ',': return makeToken(TokKind::Comma, start, start + 1);
    case ';': return makeToken(TokKind::Semicolon, start, start + 1);
    case '<':
      if (peek(1) == '=') return makeToken(TokKind::LessEq, start, start + 2);
      // 注意：`<<` 不是 SysY 运算符 —— 两个 '<' 各自成 token（最长匹配的必然结果）
      return makeToken(TokKind::Less, start, start + 1);
    case '>':
      if (peek(1) == '=') return makeToken(TokKind::GreaterEq, start, start + 2);
      return makeToken(TokKind::Greater, start, start + 1);
    case '=':
      if (peek(1) == '=') return makeToken(TokKind::EqEq, start, start + 2);
      return makeToken(TokKind::Assign, start, start + 1);
    case '!':
      if (peek(1) == '=') return makeToken(TokKind::NotEq, start, start + 2);
      return makeToken(TokKind::Not, start, start + 1);
    case '&':
      if (peek(1) == '&') return makeToken(TokKind::AmpAmp, start, start + 2);
      // SysY 没有按位与：单个 '&' 是非法字符
      return makeInvalid(start, "unexpected character");
    case '|':
      if (peek(1) == '|') return makeToken(TokKind::PipePipe, start, start + 2);
      return makeInvalid(start, "unexpected character");
    default:
      break;
  }

  // ── 非法字符：报 error + 产出 Invalid + 【继续扫描】─────────────────────
  // 典型：`@`（张量乘，不在范围内）；`"` 字符串（SysY 没有字符串字面量）；
  //       `#` `$` `?` `~` `^` `\` 与裸露的 `.`
  return makeInvalid(start, "unexpected character");
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    Token t = next();
    const bool eof = (t.kind == TokKind::EndOfFile);
    out.push_back(t);
    if (eof) break;
  }
  return out;
}

}  // namespace sysy
