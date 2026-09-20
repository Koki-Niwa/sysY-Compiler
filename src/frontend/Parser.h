// ============================================================================
// Parser —— SysY 语法分析器（S02）
//
// 本文件是"手写递归下降 + 运算符优先级爬升"的对外接口。
//
// ── 三个必须遵守的契约（phases/S02-parser.md §3.2/§9）──────────────────────
//
//  1. **无回溯**：每个文法决策都由**有界前瞻**（最多 2 个 token）确定，
//     不存在"先试着解析，失败再退回重来"。只有两处需要看第二步：
//       * `Ident` 后面是不是 `(`   → LVal 还是 Call      （parsePrimary）
//       * `Ident` 后面是不是 `[` `=` → 语句还是赋值语句 （parseStmt）
//     理由：回溯会让最坏复杂度爆炸，而 540 个用例里最长一行 86 KB。
//
//  2. **二元运算用循环而不是递归**：`a+b+c+...` 若按 `AddExp → AddExp '+' MulExp`
//     直接递归实现，树会退化成 N 层递归（86 KB 单行实测约 5000 个运算符）→ 栈溢出。
//     所以六个二元层用同一张优先级表 + 一个 while 循环（优先级爬升）。
//     **只有真正有嵌套语义的地方才递归**：括号、一元链、语句/块。
//
//  3. **出错不抛异常、不中断**：报诊断 → 恢复到同步点 → 继续解析，
//     最终仍返回一棵**尽可能完整的** AST（非 nullptr）。这是比赛"准确识别、
//     定位错误"的要求：一个文件里的多个错误要一次全报出来。
//     恢复的收敛性由 Parser::recoverToStmtEnd 保证（永远前进，不会死循环）。
//
// ── 与 S01 的接口 ─────────────────────────────────────────────────────────
//   Parser 在构造时用 `lexer.tokenize()` 一次性取完全部 token 并缓存，
//   于是"任意前瞻"只是一次数组下标（**仍然是零回溯**：决策只看 token，
//   不做"先解析、失败再回退"）。见 Parser.cpp 里 parseTopLevelItem 的注释。
//
// ── 递归深度防护 ─────────────────────────────────────────────────────────
//   递归下降对**畸形输入**可能自我放大（例如 `((((((...` 或 `!!!!...`）。
//   本实现用一个显式计数器（kMaxDepth）在超限时【报错并照常返回】——
//   **不依赖栈溢出（那是崩溃，验收要求"不崩"）**。见 Parser::DepthGuard。
// ============================================================================
#ifndef SYSY_FRONTEND_PARSER_H
#define SYSY_FRONTEND_PARSER_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/Token.h"
#include "support/SourceLoc.h"

namespace sysy {

class Lexer;
class DiagnosticEngine;

class Parser {
 public:
  // 【前置】lexer 可用（其 SourceFile 的生命期要覆盖本对象）。diag 的 SourceFile 已设置。
  // 【后置】无副作用；不抛异常。
  Parser(Lexer& lexer, DiagnosticEngine& diag);

  // 【前置】无。【后置】解析整个编译单元（到 EndOfFile 为止）。
  //   出错时：报诊断、尽量恢复、**仍返回非 nullptr 的 CompUnit**（可能是部分的）。
  std::unique_ptr<CompUnit> parseCompUnit();

  // 【前置】无。【后置】本对象报出的 error 数（不含 warning）。
  size_t errorCount() const { return errors_; }

 private:
  // ── token 访问（唯一的前瞻机制）────────────────────────────────────────
  // token 一次性取完并缓存（见 Parser.cpp 的"为什么用缓冲"注释）。
  // 必须有**任意前瞻**：`int a[3] = {…};` 与 `int f(int a) {…}` 都以
  // `KwInt Ident` 开头，要看完 `[3]` 之后的那个 token 才能分流。
  const Token& cur() const { return toks_[pos_]; }
  const Token& peek();                       // == peekAhead(1)
  TokKind curKind() const { return cur().kind; }
  TokKind peekKind();
  // 从"当前 token"往后再看 k 个（k=0 即当前）。越界返回 EndOfFile。
  const Token& peekAhead(size_t k);
  bool at(TokKind k) const { return cur().kind == k; }
  bool atPeek(TokKind k) { return peekKind() == k; }

  void advance();                            // 消费当前 token；EndOfFile 处停留不前进
  bool accept(TokKind k);                     // 命中则消费并返回 true
  bool expect(TokKind k, const char* what);   // 不命中则报错，返回 false（不消费）
  // ── 诊断 ───────────────────────────────────────────────────────────────
  void error(SourceLoc loc, const std::string& msg);
  void warn(SourceLoc loc, const std::string& msg);
  void errorHere(const std::string& msg);
  // "expected X, but got Y" 的统一措辞（与 S01 的诊断风格一致）。
  // what 是给人看的期望描述，例如 "';' after declaration" / "an expression"。
  void expected(const char* what);
  // 当前 token 的原文（拼进诊断消息用；EOF 返回 "end of file"）
  std::string curText();

  // ── 错误恢复 ───────────────────────────────────────────────────────────
  // 跳到本语句的同步点：`;` / `}` / EndOfFile（只在括号深度 0 处停止）。
  // 保证：只要没到 EndOfFile 就【至少前进一个 token】——这是"绝不卡死"的关键。
  void recoverToStmtEnd();
  // 深度超限时把 token 推到与当前层配平的 `}` 之后（保证前进，见 .cpp 注释）
  void skipToMatchingBrace();

  // ── 顶层 ───────────────────────────────────────────────────────────────
  // 解析一个顶层项（声明或函数定义）；当前 token 必须是 const/int/float/void。
  bool parseTopLevelItem(CompUnit& unit);
  // 从 toks_[i] 起跳过成对的 `[...]`，判断其后的 token 是否为 '('（任意前瞻）
  bool tokenAfterDimsIsLParenAt(size_t i);
  std::unique_ptr<FuncDef> parseFuncDef(SourceLoc loc, BType retType,
                                        const std::string& name);
  std::vector<Param> parseParams();
  bool parseParam(std::vector<Param>& out);

  // ── 声明 ───────────────────────────────────────────────────────────────
  // 在已知 `[const] BType` 前缀后解析 `VarDef {',' VarDef} ';'`。
  // 返回 nullptr 表示"不是声明"（当前 token 不是 Ident）——调用方据此报错。
  std::unique_ptr<Decl> parseDeclRest(SourceLoc loc, bool isConst, BType base);
  // Ident {'[' Exp ']'}（收尾的分隔符/等号不消费）
  void parseVarDefDims(std::vector<Dim>& dims);
  // 函数形参的数组后缀：'[' ']' {'[' Exp ']'}（第一维必须为空）
  void parseParamArrayDims(TypeSpec& type);

  // ── 语句 ───────────────────────────────────────────────────────────────
  std::unique_ptr<Stmt> parseStmt();
  std::unique_ptr<BlockStmt> parseBlock();
  std::unique_ptr<InitVal> parseInitVal();
  std::unique_ptr<Expr> parseCond();

  // ── 表达式 ─────────────────────────────────────────────────────────────
  // 优先级爬升。lhs 可以是调用方已经解析好的**前缀结果**（nullptr = 从头解析）。
  // 为什么需要它：语句层要先按 LVal 的形状探一下"这是不是赋值语句"，
  // 而那个探测已经把 `Ident`（可能还有下标链）消费掉了；把结果交回来再往下爬，
  // 就既不需要回溯、也不会重复解析。见 Parser.cpp 的 parseStmt。
  std::unique_ptr<Expr> parseExp(int minPrec, std::unique_ptr<Expr> lhs = nullptr);
  std::unique_ptr<Expr> parseUnary();
  std::unique_ptr<Expr> parsePostfix();
  std::unique_ptr<Expr> parsePrimary();

  // ── 数值（辅助值，不是常量求值）──────────────────────────────────────
  void parseIntLiteral(IntLit& lit);
  void parseFloatLiteral(FloatLit& lit);

  // ── 递归深度防护 ───────────────────────────────────────────────────────
  // kMaxDepth 的定值依据（实测，见 S02 报告 §8）：
  //   * 语料里最深的嵌套是 29_long_line.sy 的 10 层块 + 82_long_func 的 14 层括号
  //   * 这个深度下每层递归的实际栈消耗 < 1 KB（parseStmt/parseExp/parseUnary 的帧都很小）
  //     所以 4000 层 ≈ 4 MB 以内，8 MB 栈留足余量
  //   * 超过它的一律是畸形/攻击性输入：报一条 error 后【照常返回】，
  //     输出退化为一个空节点 —— **不崩**才是验收要求
  static constexpr int kMaxDepth = 4000;
  // 深度超限后，同一条诊断只报一次（否则一个病态输入能刷出几十万条诊断）
  bool depthReported_ = false;

  struct DepthGuard {
    Parser& p;
    explicit DepthGuard(Parser& parser) : p(parser) { ++p.depth_; }
    ~DepthGuard() { --p.depth_; }
    // 是否已经超限（超限时调用方立刻返回，不再继续下钻）
    bool exceeded() const { return p.depth_ > kMaxDepth; }
  };
  bool depthExceeded();   // 超限时报一次错并返回 true

  Lexer& lexer_;
  DiagnosticEngine& diag_;
  std::vector<Token> toks_;   // 全部 token（末尾必有 EndOfFile 哨兵）
  size_t pos_ = 0;            // 当前 token 下标（恒 < toks_.size()）
  int depth_ = 0;      // 当前递归深度（见 kMaxDepth）
  // 已经报过 Invalid 的 token 下标（同一位置的词法错误只报一次，见 advance()）
  std::vector<size_t> invalidReported_;
  int loopDepth_ = 0;  // 当前所处的 while 嵌套层数（break/continue 合法性用）
  size_t errors_ = 0;
};

}  // namespace sysy

#endif  // SYSY_FRONTEND_PARSER_H
