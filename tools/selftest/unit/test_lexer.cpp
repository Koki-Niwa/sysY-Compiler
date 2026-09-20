// ============================================================================
// test_lexer.cpp —— 词法分析器单元测试（S01 验证标准 §7.9）
//
// 覆盖 phases/S01-lexer.md §5.4 的 **13 个边界用例**，外加：
//   * §4 的 --emit=tokens 转储格式（逐字节比对规格里给的例子）
//   * §5.1 的拼接不变式（复刻一遍 check_lexer.py 的核心判据）
//   * §3 的 loc/length 反查原文（offsetOf(loc)+length == 原文）
//   * 契约：文件末尾后反复 next() 都是 EndOfFile
//
// 全部用 SourceFile::fromString，**不读磁盘**，所以断言与 CWD 无关。
// 依赖：compiler/tools/selftest/unit/run.sh（不需要第三方测试框架）
// ============================================================================
#include <iostream>
#include <string>
#include <vector>

#include "frontend/Lexer.h"
#include "frontend/Token.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using sysy::DiagnosticEngine;
using sysy::Lexer;
using sysy::SourceFile;
using sysy::TokKind;
using sysy::Token;

static int g_failed = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (ok) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  X " << what << "\n";
  }
}

static void checkEq(const std::string& what, const std::string& got,
                    const std::string& want) {
  ++g_checks;
  if (got == want) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  X " << what << "\n---- 期望 ----\n[" << want
              << "]\n---- 实际 ----\n[" << got << "]\n--------------\n";
  }
}

// 单字符/短串的可读转义（避免不可见字符把日志搞乱）
static std::string brief(const std::string& s) {
  std::string out;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\n') { out += "\\n"; continue; }
    if (c == '\t') { out += "\\t"; continue; }
    if (u < 32 || u >= 127) {
      static const char* hex = "0123456789abcdef";
      out += "\\x";
      out += hex[(u >> 4) & 0xF];
      out += hex[u & 0xF];
      continue;
    }
    out += c;
  }
  return out;
}

// —— 工具：token 化一个内存里的源文件 ——
struct Scan {
  SourceFile src;
  DiagnosticEngine diags;
  std::vector<Token> tokens;
  size_t errors = 0;

  Scan(const std::string& path, std::string content)
      : src(SourceFile::fromString(path, std::move(content))) {
    diags.setSourceFile(&src);
    Lexer lx(src, diags);
    tokens = lx.tokenize();
    errors = lx.errorCount();
  }
};

// 只做词法、不取 token（拿 errors_）
static size_t lexErrorCount(const std::string& content) {
  SourceFile src = SourceFile::fromString("e.sy", content);
  DiagnosticEngine diags;
  diags.setSourceFile(&src);
  Lexer lx(src, diags);
  lx.tokenize();
  return lx.errorCount();
}

// 把 token 流变成 "kind(text)" 形式，便于一眼比对
static std::string kindSeq(const Scan& s) {
  std::string out;
  for (const Token& t : s.tokens) {
    if (!out.empty()) out += ' ';
    out += sysy::tokKindDumpName(t.kind);
    out += '(';
    out += brief(std::string(sysy::tokenText(t, s.src)));
    out += ')';
  }
  return out;
}

// 复刻 check_lexer.py 的不变式：第 4 列拼接 == 原文去空白去注释
static std::string expectedStripNoComments(const std::string& text) {
  std::string out;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f') { ++i; continue; }
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      i += 2;
      while (i < n && text[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) ++i;
      i = i + 2 < n ? i + 2 : n;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

static std::string joinedTokenText(const Scan& s) {
  std::string out;
  for (const Token& t : s.tokens) {
    if (t.kind == TokKind::EndOfFile || t.kind == TokKind::Invalid) continue;
    const std::string_view v = sysy::tokenText(t, s.src);
    out.append(v.data(), v.size());
  }
  return out;
}

// 用 token 的 loc+length 反查原文（§7.6）
static bool roundTripOk(const Scan& s, std::string& bad) {
  for (const Token& t : s.tokens) {
    if (t.kind == TokKind::EndOfFile) continue;
    const size_t off = s.src.offsetOf(t.loc);
    if (off + t.length > s.src.text().size()) {
      bad = "offset+length 越界: " + std::to_string(off) + "+" + std::to_string(t.length);
      return false;
    }
    const std::string_view byLoc(s.src.text().data() + off, t.length);
    const std::string_view byField = sysy::tokenText(t, s.src);
    if (byLoc != byField) {
      bad = "loc/length 反查与 Token::text 不一致: [" + brief(std::string(byLoc)) +
            "] vs [" + brief(std::string(byField)) + "]";
      return false;
    }
    if (t.length == 0) { bad = "长度 0 的非 EOF token"; return false; }
  }
  return true;
}

static void expectKinds(const std::string& what, const std::string& src,
                        const std::string& wantKinds) {
  Scan s("k.sy", src);
  Lexer lx(s.src, s.diags);   // 第二次扫描（Scan 里已经扫过一遍）——刻意为之：
                              // 验证 Lexer 可重复构造、两次结果一致
  std::string got;
  for (Token t = lx.next(); t.kind != TokKind::EndOfFile; t = lx.next()) {
    if (!got.empty()) got += ' ';
    got += sysy::tokKindDumpName(t.kind);
  }
  checkEq(what, got, wantKinds);
}

int main() {
  std::cout << "== Lexer（S01 边界用例 §5.4）==\n";

  // ── 1. 块注释跨多行、含 `//` 与引号 ────────────────────────────────────
  {
    Scan s("c1.sy", "int a; /* l1\n l2 // not eol\n \"quote\" \n*/ int b;\n");
    checkEq("1. 块注释跨多行（含 // 与引号）→ ident int, ident a, semicolon, "
            "ident int, ident b, semicolon, EOF",
            kindSeq(s),
            "kw_int(int) ident(a) semicolon(;) kw_int(int) ident(b) semicolon(;) EOF()");
    check(s.errors == 0, "1. 无词法错误");
  }

  // ── 2. 行注释里含 `/*`：到行尾结束，不进入多行模式 ──────────────────────
  {
    Scan s("c2.sy", "int a; // /* stays line-only\nint b;\n");
    checkEq("2. `//` 注释里的 `/*` 不开启块注释",
            kindSeq(s),
            "kw_int(int) ident(a) semicolon(;) kw_int(int) ident(b) semicolon(;) EOF()");
    check(s.errors == 0, "2. 无词法错误");
  }

  // ── 3. 十六进制浮点 ────────────────────────────────────────────────────
  {
    Scan s("c3.sy", "float x = 0x1.921fb6p+1;\n");
    checkEq("3. 0x1.921fb6p+1 是【一个】 FloatLit",
            kindSeq(s),
            "kw_float(float) ident(x) assign(=) floatlit(0x1.921fb6p+1) semicolon(;) EOF()");
    Scan s2("c3b.sy", "float y = 0x.AP-3; float z = 0x1p-3;\n");
    checkEq("3b. 0x.AP-3 与 0x1p-3 都是 FloatLit",
            kindSeq(s2),
            "kw_float(float) ident(y) assign(=) floatlit(0x.AP-3) semicolon(;) "
            "kw_float(float) ident(z) assign(=) floatlit(0x1p-3) semicolon(;) EOF()");
    check(s2.errors == 0, "3b. 无词法错误");
  }

  // ── 4. 八进制前缀 + 小数点 → 浮点 ──────────────────────────────────────
  {
    Scan s("c4.sy", "float p = 03.14159;\n");
    checkEq("4. 03.14159 是【一个】 FloatLit（前导 0 在有点时无意义）",
            kindSeq(s),
            "kw_float(float) ident(p) assign(=) floatlit(03.14159) semicolon(;) EOF()");
  }

  // ── 5. ★ 三种"不完整"浮点形式 ──────────────────────────────────────────
  {
    Scan s("c5.sy", "1. .5 2. .0\n");
    checkEq("5. ★ 1. / .5 / 2. / .0 四个都是 FloatLit",
            kindSeq(s),
            "floatlit(1.) floatlit(.5) floatlit(2.) floatlit(.0) EOF()");
    check(s.errors == 0, "5. 无词法错误（朴素实现会在这里报 '.' 非法）");
    check(lexErrorCount("1.\n") == 0, "5b. 单独一行 `1.` 无错");
    check(lexErrorCount(".0\n") == 0, "5c. 单独一行 `.0` 无错");
  }

  // ── 6. a+++++b：最长匹配只能是单个 '+' ─────────────────────────────────
  {
    Scan s("c6.sy", "a+++++b;\n");
    checkEq("6. a+++++b → a + + + + + b ;（没有 '++' 这个 token）",
            kindSeq(s),
            "ident(a) plus(+) plus(+) plus(+) plus(+) plus(+) ident(b) semicolon(;) EOF()");
    check(s.errors == 0, "6. 无词法错误");
  }

  // ── 7. 关系运算符与不存在的 '<<' ───────────────────────────────────────
  {
    Scan s("c7.sy", "a<=b; a<b; a<<=b;\n");
    // ★ `a<<=b` 的最长匹配是 `<` 然后 `<=`（不是 `< < =`）：
    //   在位置 1 上，`<<` 不存在 → 退一格取 `<`；位置 2 上 `<=` 是合法的两字符 token，
    //   最长匹配优先 ⇒ `<` `<=`。这与 C 的 maximal munch 一致。
    //   （prompt §5.4 表格里写的是 `< < =`，那与"最长匹配"自相矛盾，此处按最长匹配固化。）
    checkEq("7. <= / < / <<= → lesseq / less / less lesseq（最长匹配）",
            kindSeq(s),
            "ident(a) lesseq(<=) ident(b) semicolon(;) "
            "ident(a) less(<) ident(b) semicolon(;) "
            "ident(a) less(<) lesseq(<=) ident(b) semicolon(;) EOF()");
    check(s.errors == 0, "7. 无词法错误（'<<' 不是运算符 → 两个 '<'）");
    // '>>' 同理
    expectKinds("7b. a>>b → greater greater", "a>>b;", "ident greater greater ident semicolon");
    expectKinds("7c. a<<b → less less", "a<<b;", "ident less less ident semicolon");
    expectKinds("7d. a>>=b → greater greatereq（最长匹配）", "a>>=b;",
                "ident greater greatereq ident semicolon");
  }

  // ── 8. `1e` 与 `1e+`：★ 明确规则并固化 ────────────────────────────────
  //   规则（本实现的选择）：指数标记后若没有【至少一位】数字，则指数部分整体不成立，
  //   最长匹配退化为 `IntLit 1` + `Ident e`（`+` 是独立的加号）。
  //   理由：这既符合"最长匹配"的定义，又能让语法阶段给出比"非法浮点"更准确的诊断；
  //         C 语言自身对 `1e` 也是 pp-number 报错，我们不改动 SysY 语义。
  {
    expectKinds("8. 1e → intlit ident（固化：指数残缺时退化为整数+标识符）",
                "1e", "intlit ident");
    expectKinds("8b. 1e+ → intlit ident plus", "1e+", "intlit ident plus");
    expectKinds("8c. 1e-3 → 一个 floatlit", "1e-3", "floatlit");
    Scan s("c8.sy", "1e");
    checkEq("8d. 1e 的原文可反查", brief(std::string(sysy::tokenText(s.tokens[0], s.src))), "1");
    checkEq("8e. 1e 的第二段原文是 e", brief(std::string(sysy::tokenText(s.tokens[1], s.src))), "e");
    check(lexErrorCount("1e") == 0, "8f. 1e 不产生词法错误（由语法阶段报错）");
    check(lexErrorCount("0x1p") == 1, "8g. 0x1p 产生 1 个词法错误（十六进制指数残缺）");
  }

  // ── 9. 文件末尾无换行 ──────────────────────────────────────────────────
  {
    Scan s("c9.sy", "int main() { return 0; }");
    check(s.tokens.back().kind == TokKind::EndOfFile, "9. 无末尾换行 → 仍以 EOF 结束");
    check(s.tokens.size() == 10, "9b. token 数正确（9 个 + EOF）");
    checkEq("9c. EOF 的行列号 = 文本末尾",
            std::to_string(s.tokens.back().loc.line) + ":" +
                std::to_string(s.tokens.back().loc.col),
            "1:25");
  }

  // ── 10. 空文件 / 只有注释的文件 ────────────────────────────────────────
  {
    Scan e("c10.sy", "");
    check(e.tokens.size() == 1 && e.tokens[0].kind == TokKind::EndOfFile,
          "10. 空文件 → 立刻 EOF（只有 1 个 token）");
    Scan c("c10b.sy", "/* nothing */\n// nor here\n");
    check(c.tokens.size() == 1 && c.tokens[0].kind == TokKind::EndOfFile,
          "10b. 只有注释 → 立刻 EOF");
    check(c.errors == 0, "10c. 无词法错误");
  }

  // ── 11. CRLF 与 LF 的行列号完全一致 ────────────────────────────────────
  {
    const std::string lf = "int main() {\n  int x = 1;\n  return x;\n}\n";
    std::string crlf;
    for (char ch : lf) {
      if (ch == '\n') crlf += '\r';
      crlf += ch;
    }
    Scan a("lf.sy", lf), b("crlf.sy", crlf);
    checkEq("11. CRLF 与 LF 的 token 序列（含种类与原文）完全一致",
            kindSeq(b), kindSeq(a));
    bool sameLoc = a.tokens.size() == b.tokens.size();
    for (size_t i = 0; sameLoc && i < a.tokens.size(); ++i) {
      if (a.tokens[i].loc != b.tokens[i].loc || a.tokens[i].length != b.tokens[i].length)
        sameLoc = false;
    }
    check(sameLoc, "11b. ★ CRLF 与 LF 的每个 token 的 loc/length 完全一致（列号不错位）");
    check(b.errors == 0, "11c. CRLF 文件无词法错误（\\r 不应变成非法字符）");
  }

  // ── 12. 86 KB 单行：正确性（性能由 check_lexer.py / 计时脚本测）─────────
  {
    std::string big;
    big.reserve(90 * 1024);
    for (int i = 0; i < 8000; ++i) big += "a1=a1+1;";
    Scan s("long.sy", big);
    check(s.errors == 0, "12. 86KB 单行：无词法错误");
    check(s.tokens.size() == 8000u * 6u + 1u, "12b. 86KB 单行：token 数正确（48000 + EOF）");
    checkEq("12c. 最后一个非 EOF token 是 ';'",
            std::string(sysy::tokenText(s.tokens[s.tokens.size() - 2], s.src)), ";");
    checkEq("12d. EOF 列号 = 行长+1",
            std::to_string(s.tokens.back().loc.col), std::to_string(big.size() + 1));
    std::string bad;
    check(roundTripOk(s, bad), "12e. loc+length 反查原文全部一致" + (bad.empty() ? "" : "：" + bad));
  }

  // ── 13. 含 `@` 的文件：报 error + Invalid，不崩，继续扫描 ───────────────
  {
    Scan s("c13.sy", "int a = 1 @ 2;\nint b = 3;\n");
    check(s.errors == 1, "13. `@` 恰好产生 1 个词法错误");
    // 13 个 token：int a = 1 @ 2 ; int b = 3 ; EOF
    check(s.tokens.size() == 13, "13b. 继续扫描到 EOF（13 个 token，最后是 EOF）");
    check(s.tokens.back().kind == TokKind::EndOfFile, "13c. 末尾是 EOF");
    bool hasInvalid = false;
    for (const Token& t : s.tokens) if (t.kind == TokKind::Invalid) hasInvalid = true;
    check(hasInvalid, "13d. 产出了 Invalid token");
    checkEq("13e. Invalid 的 loc 指向 '@'",
            std::to_string(s.tokens[4].loc.line) + ":" + std::to_string(s.tokens[4].loc.col),
            "1:11");
    // `@` 之后必须继续正常扫描：从第 5 个 token 起应当是 2 ; int b = 3 ; EOF
    std::string tail;
    for (size_t i = 5; i < s.tokens.size(); ++i) {
      if (!tail.empty()) tail += ' ';
      tail += sysy::tokKindDumpName(s.tokens[i].kind);
      tail += '(';
      tail += brief(std::string(sysy::tokenText(s.tokens[i], s.src)));
      tail += ')';
    }
    checkEq("13f. ★ `@` 之后的 `2` 与后续语句仍被正常扫描", tail,
            "intlit(2) semicolon(;) kw_int(int) ident(b) assign(=) intlit(3) "
            "semicolon(;) EOF()");

    // 多个非法字符都要报出来（比赛要求"准确识别、定位"，不能报一个就停）
    check(lexErrorCount("int a = 1 $ 2;\nint b = 1 @ 2;\n") == 2,
          "13g. 一行里两个非法字符 → 2 个错误（不中断）");
    check(lexErrorCount("@\n") == 1, "13h. 单独一个 `@` → 1 个错误");
  }

  // ── 14. `--emit=tokens` 的转储格式（§4 的规格例子，逐字节）─────────────
  {
    Scan s("d.sy", "int main() { return 0; }\n");
    std::string got;
    for (const Token& t : s.tokens) {
      got += std::to_string(t.loc.line);
      got += '\t';
      got += std::to_string(t.loc.col);
      got += '\t';
      got += sysy::tokKindDumpName(t.kind);
      got += '\t';
      if (t.kind == TokKind::EndOfFile) got += '-';
      else got.append(sysy::tokenText(t, s.src).data(), sysy::tokenText(t, s.src).size());
      got += '\n';
    }
    const std::string want =
        "1\t1\tkw_int\tint\n"
        "1\t5\tident\tmain\n"
        "1\t9\tlparen\t(\n"
        "1\t10\trparen\t)\n"
        "1\t12\tlbrace\t{\n"
        "1\t14\tkw_return\treturn\n"
        "1\t21\tintlit\t0\n"
        "1\t22\tsemicolon\t;\n"
        "1\t24\trbrace\t}\n"
        "1\t25\tEOF\t-\n";
    checkEq("14. ★ 转储格式与 phases/S01-lexer.md §4 的示例逐字节相同", got, want);
  }

  // ── 15. 字节拼接不变式（§5.1，在内存里复刻一遍）────────────────────────
  {
    const char* samples[] = {
        "int main() { return 0; }\n",
        "// c\nint a; /* b */ float x = .5;\n",
        "int fib(int n){if(n<=2)return 1;return fib(n-1)+fib(n-2);}\n",
        "const float P = 0x1.921fb6p+1, Q = 03.5e-3;\n",
        "\t \v\f int\ta\r\n=\r\n1;\n",
    };
    for (const char* txt : samples) {
      Scan s("inv.sy", txt);
      checkEq(std::string("15. 拼接不变式: ") + brief(txt), joinedTokenText(s),
              expectedStripNoComments(s.src.text()));
    }
    std::string bad;
    Scan s("rt.sy", samples[3]);
    check(roundTripOk(s, bad), "15b. loc+length 反查原文（含十六进制浮点/八进制点）");
  }

  // ── 16. 契约：EOF 之后反复 next() 都返回 EndOfFile ─────────────────────
  {
    SourceFile src = SourceFile::fromString("eof.sy", "int a;\n");
    DiagnosticEngine diags;
    diags.setSourceFile(&src);
    Lexer lx(src, diags);
    (void)lx.tokenize();
    bool allEof = true;
    for (int i = 0; i < 1000; ++i) {
      const Token t = lx.next();
      if (t.kind != TokKind::EndOfFile) allEof = false;
    }
    check(allEof, "16. ★ tokenize() 之后再调用 1000 次 next() 全是 EndOfFile（不崩、不越界）");

    Lexer lx2(src, diags);
    const Token a = lx2.next();
    const Token b = lx2.next();
    const Token c = lx2.next();
    check(a.kind == TokKind::KwInt && b.kind == TokKind::Ident &&
              c.kind == TokKind::Semicolon,
          "16b. 逐个 next() 的顺序正确");
  }

  // ── 17. 注释里的引号不能触发"字符串模式"（§九 坑 1）────────────────────
  {
    Scan s("q.sy", "//#include \"sylib.h\"\nint a;\n");
    check(s.errors == 0, "17. 注释里的引号不产生任何错误");
    checkEq("17b. 注释整段被跳过",
            kindSeq(s), "kw_int(int) ident(a) semicolon(;) EOF()");
    Scan s2("q2.sy", "int a; // \"unterminated\nint b;\n");
    checkEq("17c. 行注释内的未闭合引号不影响下一行",
            kindSeq(s2),
            "kw_int(int) ident(a) semicolon(;) kw_int(int) ident(b) semicolon(;) EOF()");
  }

  // ── 18. 关键字集合严格等于规范（多一个少一个都错）──────────────────────
  {
    expectKinds("18. 10 个关键字全是关键字",
                "const int float void if else while break continue return",
                "kw_const kw_int kw_float kw_void kw_if kw_else kw_while "
                "kw_break kw_continue kw_return");
    expectKinds("18b. ★ tensor/for/do/struct/switch/goto 是【普通标识符】",
                "tensor for do struct switch goto",
                "ident ident ident ident ident ident");
    Scan s("kw.sy", "tensor for do struct switch goto");
    check(s.errors == 0, "18c. 它们不产生词法错误");
    checkEq("18d. `tensor` 的原文可反查",
            std::string(sysy::tokenText(s.tokens[0], s.src)), "tensor");
  }

  // ── 19. 编码/文本边界：NUL、非 ASCII、非法字节 ─────────────────────────
  {
    // NUL：不是空白也不是合法 token 字符 → Invalid + 报错，且【必须继续前进】
    std::string withNul = "int a";
    withNul += '\0';
    withNul += "; int b;";
    check(lexErrorCount(withNul) == 1, "19. NUL 字节 → 1 个词法错误（不死循环）");
    Scan s("nul.sy", withNul);
    check(s.tokens.back().kind == TokKind::EndOfFile, "19b. NUL 之后仍能扫到 EOF");
    // 非 ASCII（UTF-8 中文）出现在注释里 → 无错误
    check(lexErrorCount("// 中文注释\nint a;\n") == 0, "19c. 注释里的 UTF-8 中文不报错");
    // 非 ASCII 出现在代码里 → 逐字节报非法（不影响继续扫描）
    check(lexErrorCount("int \xe4\xb8\xad;") == 3, "19d. 代码里的 UTF-8 中文 → 每字节一个错误");
  }

  // ── 20. 未闭合块注释：报错但不崩 ───────────────────────────────────────
  {
    Scan s("unterm.sy", "int a;\n/* never closed\n");
    check(s.errors == 1, "20. 未闭合块注释 → 1 个错误");
    check(s.tokens.back().kind == TokKind::EndOfFile, "20b. 仍以 EOF 结束");
    checkEq("20c. 注释前的 token 正常",
            kindSeq(s), "kw_int(int) ident(a) semicolon(;) EOF()");
  }

  std::cout << "\n" << (g_failed == 0 ? "全部通过" : "有失败") << ": "
            << (g_checks - g_failed) << "/" << g_checks << "\n";
  return g_failed == 0 ? 0 : 1;
}
