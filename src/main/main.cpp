// ============================================================================
// main —— 命令行入口（P00 prompt §2）
//
//   compiler <input> -o <output> [选项]
//     -o <file>          输出文件（必填）
//     --emit=<kind>      llvm-ir（默认）| tokens | ast | sema | initplan | structured-ir | nothing
//     --from-ast         输入是 --emit=ast 产出的 S-表达式文本（往返验证用）
//     -O1                开启性能优化（S00 只记录，不实现）
//     -S                 比赛调用形式，必须接受；本阶段忽略其含义
//     -h, --help / -v, --version
//
// 【S00】解析命令行 → 读文件 → 按 --emit 输出。
// 【S01】--emit=tokens 已是【真实实现】（词法器）。
// 【S02】--emit=ast 与 --from-ast 已是【真实实现】（语法分析器 + AST 打印/读取器）。
// 【S03】--emit=sema 已是【真实实现】（语义分析 + 类型注解转储）。
// 【S04】--emit=initplan 已是【真实实现】（初始化器降级 → 初始化计划）。
//        其余 emit 仍是占位。
//
// 铁律 4：编译器本体不调用任何外部程序（这里没有 system/exec/fork）。
// ============================================================================
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/AstPrinter.h"
#include "frontend/InitLowering.h"
#include "frontend/InitLoweringDump.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "frontend/SemaDump.h"
#include "frontend/Token.h"
#include "support/Diagnostic.h"
#include "support/Errors.h"
#include "support/SourceFile.h"

#ifndef SYSY_COMPILER_VERSION
#define SYSY_COMPILER_VERSION "0.0.0-dev"
#endif

namespace {

using sysy::CompUnit;
using sysy::DiagnosticEngine;
using sysy::DiagLevel;
using sysy::IOError;
using sysy::Lexer;
using sysy::Parser;
using sysy::SourceFile;
using sysy::SourceLoc;
using sysy::Token;
using sysy::TokKind;
using sysy::UsageError;

// 退出码约定（run_tests.sh 依赖 0/非 0 区分；1 专指"编译错误"）
enum ExitCode {
  kOk = 0,
  kCompileError = 1,
  kUsageError = 2,
  kIOError = 3,
  kInterrupted = 130,
};

enum class EmitKind { LlvmIr, Tokens, Ast, Sema, InitPlan, StructuredIr, Nothing };

const char* toString(EmitKind k) {
  switch (k) {
    case EmitKind::LlvmIr:       return "llvm-ir";
    case EmitKind::Tokens:       return "tokens";
    case EmitKind::Ast:          return "ast";
    case EmitKind::Sema:         return "sema";
    case EmitKind::InitPlan:     return "initplan";
    case EmitKind::StructuredIr: return "structured-ir";
    case EmitKind::Nothing:      return "nothing";
  }
  return "llvm-ir";
}

bool parseEmitKind(const std::string& s, EmitKind& out) {
  if (s == "llvm-ir")       { out = EmitKind::LlvmIr;       return true; }
  if (s == "tokens")        { out = EmitKind::Tokens;       return true; }
  if (s == "ast")           { out = EmitKind::Ast;          return true; }
  if (s == "sema")          { out = EmitKind::Sema;         return true; }
  if (s == "initplan")      { out = EmitKind::InitPlan;     return true; }
  if (s == "structured-ir") { out = EmitKind::StructuredIr; return true; }
  if (s == "nothing")       { out = EmitKind::Nothing;      return true; }
  return false;
}

struct Options {
  std::string input;
  std::string output;          // -o 必填
  EmitKind emit = EmitKind::LlvmIr;
  bool haveOutput = false;
  int optLevel = 0;            // -O1 → 1（S00 只记录）
  bool structured = true;      // S05 起：结构化 IR 层开关（默认开）
  bool optimize = false;       // --optimize：与 -O1 等价的别名（run_tests.sh 用它）
  bool toyBackend = false;     // --toy-backend：S11b 起表示"用自研降级器"；当前只接受
  bool fromAst = false;        // --from-ast：输入是 S-表达式（不是 SysY 源码）
  bool verbose = false;
};

const char* kHelpText =
    "SysY compiler (front/middle end) —— SysY -> LLVM IR subset\n"
    "\n"
    "用法: compiler <input> -o <output> [选项]\n"
    "\n"
    "选项:\n"
    "  -o <file>          输出文件（必填）\n"
    "  --emit=<kind>      产物类型: llvm-ir（默认）| tokens | ast | sema | initplan\n"
    "                     | structured-ir | nothing\n"
    "  --from-ast         输入是 --emit=ast 的产物（S-表达式），而不是 SysY 源码\n"
    "                     （S02 起用于验证：source --emit=ast --from-ast --emit=ast）\n"
    "                     --emit=sema 时同样接受；产物不保证能被读回（单向视图）\n"
    "  -O0 / -O1          优化级别（-O1 目前只记录，S17 起生效）\n"
    "  --optimize         -O1 的别名（tools/run_tests.sh 使用长选项名）\n"
    "  -S                 接受但不解释（比赛调用形式: compiler a.sy -S -o a.s）\n"
    "  --structured       使用结构化 IR 层（默认）\n"
    "  --no-structured    不走结构化 IR 层（供回归对照用）\n"
    "  --toy-backend      接受但不解释（测试链路选择：走自研降级器而非 clang）\n"
    "  --verbose          打印处理过程\n"
    "  -h, --help         显示本帮助并退出\n"
    "  -v, --version      显示版本并退出\n"
    "\n"
    "输入扩展名 .sy 与 .sysy 都接受。\n"
    "\n"
    "退出码: 0 成功 | 1 编译错误 | 2 命令行用法错误 | 3 文件读写错误\n";

// 解析命令行。返回 false 表示"已处理完（--help/--version），直接退出 0"。
bool parseCommandLine(int argc, char** argv, Options& opts) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::vector<std::string> positionals;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];

    if (a == "-h" || a == "--help") {
      std::cout << kHelpText;
      return false;
    }
    if (a == "-v" || a == "--version") {
      std::cout << "sysy-compiler " << SYSY_COMPILER_VERSION
                << " (front/middle end; stage S04 — init lowering)\n";
      return false;
    }
    if (a == "-o") {
      if (i + 1 >= args.size()) throw UsageError("option '-o' requires a file argument");
      opts.output = args[++i];
      opts.haveOutput = true;
      continue;
    }
    if (a.rfind("-o", 0) == 0 && a.size() > 2) {   // -ofile
      opts.output = a.substr(2);
      opts.haveOutput = true;
      continue;
    }
    if (a == "--emit") {
      if (i + 1 >= args.size()) throw UsageError("option '--emit' requires a value");
      if (!parseEmitKind(args[++i], opts.emit))
        throw UsageError("unknown --emit kind '" + args[i] +
                         "' (expected: llvm-ir | tokens | ast | sema | structured-ir | nothing)");
      continue;
    }
    if (a.rfind("--emit=", 0) == 0) {
      const std::string v = a.substr(7);
      if (!parseEmitKind(v, opts.emit))
        throw UsageError("unknown --emit kind '" + v +
                         "' (expected: llvm-ir | tokens | ast | sema | structured-ir | nothing)");
      continue;
    }
    if (a == "-O0") { opts.optLevel = 0; continue; }
    if (a == "-O1") { opts.optLevel = 1; continue; }
    if (a == "-O2" || a == "-O3" || a == "-Os") {
      throw UsageError("option '" + a + "' is not supported (only -O0/-O1)");
    }
    if (a == "-S") { continue; }              // 比赛调用形式：接受，忽略含义
    if (a == "-c" || a == "-E") { continue; }  // 同理：不为它们做事，但不报错
    if (a == "--structured")    { opts.structured = true;  continue; }
    if (a == "--no-structured") { opts.structured = false; continue; }
    // --optimize 是 -O1 的别名（tools/run_tests.sh 用长选项名）。
    if (a == "--optimize")      { opts.optLevel = 1; opts.optimize = true; continue; }
    // --toy-backend 由 tools/run_tests.sh 传入，用于让链路走【自研降级器】而非 clang。
    // 那是【测试链路】的选择，不是编译器的行为；编译器只需接受它（同 -S 的处理方式）。
    if (a == "--toy-backend")   { opts.toyBackend = true; continue; }
    if (a == "--from-ast")      { opts.fromAst = true; continue; }
    if (a == "--verbose")       { opts.verbose = true; continue; }

    if (!a.empty() && a[0] == '-' && a != "-") {
      throw UsageError("unknown option '" + a + "' (try --help)");
    }
    positionals.push_back(a);
  }

  if (positionals.empty()) throw UsageError("no input file given (try --help)");
  if (positionals.size() > 1) {
    throw UsageError("multiple input files given: '" + positionals[0] + "', '" +
                     positionals[1] + "' (expected exactly one)");
  }
  opts.input = positionals[0];

  if (!opts.haveOutput) throw UsageError("missing required option '-o <file>'");
  return true;
}

void writeWholeFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw IOError("cannot open output file '" + path + "' for writing");
  out << content;
  out.flush();
  if (!out) throw IOError("error while writing '" + path + "'");
  out.close();
  if (!out) throw IOError("error while closing '" + path + "'");
}

// ============================================================================
// --emit=tokens 的转储格式（phases/S01-lexer.md §4，**机器可校验，必须严格**）
//
//   <行>\t<列>\t<种类>\t<原文或长度>
//
//   * 每行一个 token，制表符分隔，最后一行固定是 `<行>\t<列>\tEOF\t-`
//   * 第 4 列放【原文】而不是"长度"：这样 check_lexer.py 能直接做
//     "第 4 列按顺序拼接 == 原文去空白去注释" 的逐字节自校验，不需要理解语义
//   * **不写任何注释头/统计行** —— 拼接不变式要求每一行都是 token
//
// 📌 Invalid token【照常输出】：它的原文就是那个非法字符（`@` / `$` …），
//    而"去空白去注释"的期望串里【也保留】它 —— 所以拼接不变式对这类文件同样成立。
//    错误另经诊断（stderr）报出，退出码为 1。
//    ⚠️ 曾有一版注释写"Invalid 不输出"，那是错的：
//       dump 格式是对外契约（--emit=tokens），照注释去"修正"代码会真的破坏不变式。
// ============================================================================
std::string renderTokens(const SourceFile& src, DiagnosticEngine& diags) {
  Lexer lexer(src, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  std::string out;
  // 粗估：平均每行 24 字节（86 KB 单行也不会有几万 token 以外的意外）
  out.reserve(tokens.size() * 24 + 16);
  char buf[32];
  for (const Token& tok : tokens) {
    // 行号
    std::snprintf(buf, sizeof(buf), "%u", tok.loc.line);
    out += buf;
    out += '\t';
    // 列号
    std::snprintf(buf, sizeof(buf), "%u", tok.loc.col);
    out += buf;
    out += '\t';
    // 种类
    out += tokKindDumpName(tok.kind);
    out += '\t';
    // 原文
    if (tok.kind == TokKind::EndOfFile) {
      out += '-';
    } else {
      const std::string_view text = tokenText(tok, src);
      out.append(text.data(), text.size());
    }
    out += '\n';
  }
  return out;
}

// ============================================================================
// --emit=ast 的真实实现（S02）
//
// 两条输入路径：
//   源  →  Lexer → Parser → AST      （默认）
//   S-表达式 → parseAstText → AST    （--from-ast，供往返验证）
// 两条路径共用同一个打印器 —— 这正是轨 A（往返）成立的前提。
// ============================================================================
std::string renderAst(const SourceFile& src, DiagnosticEngine& diags, bool fromAst) {
  std::unique_ptr<CompUnit> unit;
  if (fromAst) {
    // 输入是 S-表达式文本。**不经过 Lexer**：AST 文本不是 SysY（prompt §6.1）。
    // ⚠️ SourceFile 的生命期由 main 持有并覆盖整个 emit 过程，所以字面量节点里
    //    指向 src.text() 的 string_view 一直有效。
    unit = sysy::parseAstText(src.text(), diags);
  } else {
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    unit = parser.parseCompUnit();
  }
  if (unit == nullptr) return std::string();   // 契约上不会发生（Parser 保证非空）
  return sysy::printAst(*unit);
}

// ============================================================================
// --emit=sema 的真实实现（S03）
//
//   `--emit=ast` 的产物是**冻结契约**（490 个文件的 SHA-256 基线 + 490 次
//   往返），所以 Sema **绝不允许**在 `--emit=ast` 的路径上跑：
//   `--emit=ast` 在打印之后立刻结束，树里没有 `Cast`、也没有类型注解。
//   （prompt §五 的第 1 条硬约束："--emit=ast 的输出一个字节都不许变"。）
//
//   `--emit=sema` = 同一棵树 + Sema 就地改写（填类型、插 Cast）+ 带注解的转储。
//   它的产物**不要求**能被 `--from-ast` 读回（单向的调试/验证视图）。
// ============================================================================
std::string renderSema(const SourceFile& src, DiagnosticEngine& diags, bool fromAst) {
  std::unique_ptr<CompUnit> unit;
  if (fromAst) {
    unit = sysy::parseAstText(src.text(), diags);
  } else {
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    unit = parser.parseCompUnit();
  }
  if (unit == nullptr) return std::string();   // 契约上不会发生（Parser 保证非空）
  sysy::runSema(*unit, diags);
  return sysy::printSemaDump(*unit);
}

// ============================================================================
// --emit=initplan 的真实实现（S04）
//
//   `--emit=sema` 的产物也是**冻结契约**（样例对 + 490 个文件的类型不变式），
//   所以 initplan **不能**在 sema 的路径上顺手加东西：它自己一条流水线
//   （parse → Sema → InitLowering → dump）。
//
//   ★ Sema 必须跑：初始化计划里的 `StoreExpr` 子节点要用 Sema 插好的
//     类型注解与 `Cast` 打印（§4.3："沿用 --emit=sema 的表达式格式"），
//     而且 `ConstEvaluator` 查"符号常量"要靠 Sema 建的 consts_ 表。
//   ★ Sema 报错时**照常产出计划**（与 tokens/ast/sema 一样：dump 是"可诊断的
//     中间结果"，丢掉它反而无从定位）；错误通过 stderr + 退出码表达。
//     InitLowering 对残缺树是防御性的（处处带边界检查），不会崩。
// ============================================================================
std::string renderInitPlan(const SourceFile& src, DiagnosticEngine& diags, bool fromAst) {
  std::unique_ptr<CompUnit> unit;
  if (fromAst) {
    unit = sysy::parseAstText(src.text(), diags);
  } else {
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    unit = parser.parseCompUnit();
  }
  if (unit == nullptr) return std::string();   // 契约上不会发生（Parser 保证非空）
  sysy::Sema sema(diags);
  sema.run(*unit);
  const sysy::InitPlan plan = sysy::runInitLowering(*unit, sema);
  return sysy::printInitPlanDump(plan);
}

// —— 尚未实现的 emit 模式的占位产物 ——
std::string renderPlaceholder(EmitKind kind, const Options& opts, const SourceFile& src) {
  std::string out;
  out += "; SysY compiler (front/middle end) —— stage S03 (type system + sema)\n";
  out += "; input:  " + src.path() + "\n";
  out += "; emit:   " + std::string(toString(kind)) + "\n";
  out += "; lines:  " + std::to_string(src.lineCount()) + "\n";
  out += std::string("; -O1:    ") +
         (opts.optLevel >= 1 ? "requested (not implemented yet)" : "off") + "\n";
  out += std::string("; structured IR layer: ") + (opts.structured ? "on" : "off") + "\n";
  out += ";\n";
  out += "; 本阶段真实可用：--emit=tokens（词法）、--emit=ast / --from-ast（语法+AST）、\n";
  out += ";               --emit=sema（语义分析 + 类型注解转储）、\n";
  out += ";               --emit=initplan（初始化计划）。\n";
  out += "; IR 尚未实现（S05 起）。\n";
  out += "; 交给后端的 .ll 契约（TESTING-GUIDE §3.3）：目标无关 ——\n";
  out += ";   不带 target triple、不带 target datalayout、\n";
  out += ";   函数属性里不带 target-cpu / target-features。\n";
  out += "; 目标由降级阶段（llc -mtriple / 自研后端）决定。\n";
  return out;
}

void ensureReadable(const SourceFile& src) {
  // 只做 I/O（读文件 + CRLF 规范化）。解析由 renderAst / renderTokens 各自负责。
  (void)src.text();
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  DiagnosticEngine diags;

  try {
    if (!parseCommandLine(argc, argv, opts)) return kOk;
  } catch (const UsageError& e) {
    std::cerr << "compiler: error: " << e.what() << "\n";
    return kUsageError;
  }

  // ★ SourceFile 的生命期必须覆盖 catch 块：DiagnosticEngine 持有它的【非 owning】指针，
  //   而 IO 错误正是在 try 内抛出、在 catch 里渲染诊断。放在 try 内会在离开作用域后
  //   留下悬垂指针（实测：渲染时 std::bad_alloc → abort）。见 S00 审查报告 F3。
  std::unique_ptr<SourceFile> srcHolder;
  try {
    srcHolder = std::make_unique<SourceFile>(SourceFile::load(opts.input));
    const SourceFile& src = *srcHolder;
    diags.setSourceFile(&src);
    ensureReadable(src);

    if (opts.verbose) {
      std::cerr << "compiler: input=" << src.path()
                << " emit=" << toString(opts.emit)
                << " output=" << opts.output
                << " O" << opts.optLevel << "\n";
    }

    if (opts.emit != EmitKind::Nothing) {
      std::string artifact;
      switch (opts.emit) {
        case EmitKind::Tokens:
          // ★ S01 的真实产物：词法分析（Token 转储，机器可校验）
          artifact = renderTokens(src, diags);
          break;
        case EmitKind::Ast:
          // ★ S02 的真实产物：语法分析 + AST（S-表达式）
          artifact = renderAst(src, diags, opts.fromAst);
          break;
        case EmitKind::Sema:
          // ★ S03 的真实产物：语义分析 + 类型注解转储
          artifact = renderSema(src, diags, opts.fromAst);
          break;
        case EmitKind::InitPlan:
          // ★ S04 的真实产物：初始化器降级 → 初始化计划（§4.3 的冻结格式）
          artifact = renderInitPlan(src, diags, opts.fromAst);
          break;
        case EmitKind::LlvmIr:
        case EmitKind::StructuredIr:
          artifact = renderPlaceholder(opts.emit, opts, src);
          break;
        case EmitKind::Nothing:
          break;
      }
      // 【即使有词法错误也照常写出产物】：dump 是"可诊断的中间结果"，
      // 丢掉它反而让 check_lexer.py 无从定位；错误通过 stderr + 退出码表达。
      writeWholeFile(opts.output, artifact);
    }
  } catch (const IOError& e) {
    // IO 错误统一走一条路径：格式为 `file: error: msg`（line 0 = 无位置信息）。
    // 无论哪条分支，打印前都要先解除对 SourceFile 的引用。
    diags.setSourceFile(nullptr);
    if (diags.totalCount() == 0) {
      std::cerr << opts.input << ": error: " << e.what() << "\n";
    } else {
      diags.report(DiagLevel::Error, SourceLoc(0, 0), e.what());
      diags.printAll(std::cerr, false);
    }
    return kIOError;
  }

  if (diags.hasError()) {
    diags.printAll(std::cerr, false);
    return kCompileError;
  }
  return kOk;
}
