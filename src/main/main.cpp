// ============================================================================
// main —— 命令行入口（P00 prompt §2）
//   …（此处原有 19 行说明，已并入 S06 报告与设计文档）
// ============================================================================
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "main/EmitText.h"

#include "frontend/Ast.h"
#include "frontend/AstPrinter.h"
#include "frontend/InitLowering.h"
#include "frontend/InitLoweringDump.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "frontend/SemaDump.h"
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "structured/AllocaHoist.h"
#include "structured/FlattenCFG.h"
#include "structured/IRGen.h"
#include "structured/LoopNormalize.h"
#include "structured/StructuredDump.h"
#include "structured/StructuredReader.h"
#include "structured/StructuredVerifier.h"
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

enum class EmitKind { LlvmIr, Tokens, Ast, Sema, InitPlan, StructuredIr, FlatIr, Nothing };

const char* toString(EmitKind k) {
  switch (k) {
    case EmitKind::LlvmIr:       return "llvm-ir";
    case EmitKind::Tokens:       return "tokens";
    case EmitKind::Ast:          return "ast";
    case EmitKind::Sema:         return "sema";
    case EmitKind::InitPlan:     return "initplan";
    case EmitKind::StructuredIr: return "structured-ir";
    case EmitKind::FlatIr:       return "flat-ir";
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
  if (s == "flat-ir")       { out = EmitKind::FlatIr;       return true; }
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
  bool fromStructured = false;  // --from-structured：输入是结构化 IR 文本
  bool fromFlat = false;        // --from-flat：输入是平面 IR 文本（S06 轨 A）
  bool flatStats = false;       // --dump-flat-stats：把平面 IR 统计打到 stderr
  bool normalize = false;       // --normalize：跑 LoopNormalize + AllocaHoist（**默认关**）
  bool loopStats = false;       // --dump-loopnorm-stats：把规范化统计打到 stderr
  bool verbose = false;
};

// 解析命令行。返回 false 表示"已处理完（--help/--version），直接退出 0"。
bool parseCommandLine(int argc, char** argv, Options& opts) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::vector<std::string> positionals;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];

    if (a == "-h" || a == "--help") {
      std::cout << sysy::mainstage::helpText();
      return false;
    }
    if (a == "-v" || a == "--version") {
      std::cout << "sysy-compiler " << SYSY_COMPILER_VERSION
                << " (front/middle end; stage S05 — structured IR + IRGen)\n";
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
                         "' (expected: llvm-ir | tokens | ast | sema | initplan | structured-ir"
                         " | flat-ir | nothing)");
      continue;
    }
    if (a.rfind("--emit=", 0) == 0) {
      const std::string v = a.substr(7);
      if (!parseEmitKind(v, opts.emit))
        throw UsageError("unknown --emit kind '" + v +
                         "' (expected: llvm-ir | tokens | ast | sema | initplan | structured-ir"
                         " | flat-ir | nothing)");
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
    // --from-structured：输入是结构化 IR 文本（S05 轨 A 的"读回"路径）
    if (a == "--from-structured") { opts.fromStructured = true; continue; }
    // ★ S06：--from-flat（输入是平面 IR 文本）与 --dump-flat-stats（统计走 stderr）
    if (a == "--from-flat")     { opts.fromFlat = true; continue; }
    if (a == "--dump-flat-stats") { opts.flatStats = true; continue; }
    // ★ S05b：循环规范化开关（**默认关**：不带它时 `--emit=structured-ir` 的
    //   输出必须与 S05 的样例对逐字节相同 —— 那是冻结契约）。
    if (a == "--normalize")     { opts.normalize = true; continue; }
    // ★ S05b：把"哪些循环被规范化、哪些没有、为什么"打到 **stderr**。
    //   为什么不写进 dump：dump 是冻结契约，多一行就破坏逐字节比对；
    //   而不留痕迹又会让"静默不规范化"无法发现（prompt §3.3）。
    if (a == "--dump-loopnorm-stats") { opts.loopStats = true; continue; }
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
//   …（此处原有 13 行说明，已并入 S06 报告与设计文档）
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
//   …（此处原有 5 行说明，已并入 S06 报告与设计文档）
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
//   …（此处原有 8 行说明，已并入 S06 报告与设计文档）
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
//   …（此处原有 11 行说明，已并入 S06 报告与设计文档）
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

// ============================================================================
// --emit=structured-ir 的真实实现（S05）
//   …（此处原有 10 行说明，已并入 S06 报告与设计文档）
// ===========================================================================
std::string baseNameOf(const std::string& path) {
  const size_t p = path.find_last_of("/\\");
  return (p == std::string::npos) ? path : path.substr(p + 1);
}

std::string renderStructuredIr(const SourceFile& src, DiagnosticEngine& diags, bool fromAst,
                               bool normalize, bool loopStats) {
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
  sysy::sir::Arena arena;
  // ⚠️ Sema 的生命期必须覆盖 IRGen（IRGen 要用 `Cast`/类型注解，但不再查符号表）
  sysy::sir::Op* mod = sysy::sir::buildModule(arena, *unit, plan, baseNameOf(src.path()), diags);
  // ★ S05b：`--normalize` 的两件事（**验收口径不同**，prompt §二）：
  //   ① 循环规范化 —— 只对满足成功条件的 `while` 生效；**不触发的循环零改动**；
  //   ② `alloca` 提升 —— 每个函数都做（它是后端不变量 2 的前提）。
  //   顺序：先规范化（它会改写体），再提升（只动入口 Region 的顺序）。
  std::string statsText;
  if (normalize) {
    sysy::sir::LoopNormStats ls;
    mod = sysy::sir::normalizeLoops(mod, arena, ls);
    const size_t moved = sysy::sir::hoistAllocas(mod);
    statsText = sysy::sir::formatLoopNormStats(ls);
    statsText += "rolled-back " + std::to_string(ls.rolledBack) + "\n";
    statsText += "allocas-hoisted " + std::to_string(moved) + "\n";
  }
  // ★ 自查：结构化 IR 生成后立刻验六条不变式（I1–I6 + use-def + 指令集封闭/
  //   终结符匹配/GEP 标记）。**失败即报 error 并退出码 1**（prompt §九.6：
  //   不允许"静默产出坏 IR"）。这就是"IRGen 直接产出合法 IR"的落地保证。
  //   ★ S05b 起 I1/I2/I3 **不再平凡**（IR 里真的有 `ForOp` 了）。
  const std::vector<sysy::sir::Violation> vs = sysy::sir::verifyModule(mod);
  if (!vs.empty()) {
    std::string msg = "结构化 IR 违反不变量（" + std::to_string(vs.size()) + " 条）：\n";
    for (size_t i = 0; i < vs.size() && i < 10; ++i) {
      msg += "  [";
      msg += vs[i].invariant;
      msg += "] ";
      if (vs[i].loc.line != 0) msg += "line " + std::to_string(vs[i].loc.line) + ": ";
      msg += vs[i].message;
      msg += '\n';
    }
    diags.report(DiagLevel::Error, sysy::SourceLoc(0, 0), "E-STRUCT-VERIFY", msg);
  }
  // ★ S05b：统计**绝不进 dump**（dump 是与 S05 样例对的冻结契约），
  //   走 stderr。放在最后：即使不变量报红，统计照样打出来（可定位）。
  if (loopStats && normalize) std::cerr << statsText;
  return sysy::sir::dumpModule(mod);
}

// ============================================================================
// --from-structured 的真实实现（S05 轨 A）
//   …（此处原有 5 行说明，已并入 S06 报告与设计文档）
// ============================================================================
std::string renderFromStructured(const SourceFile& src, DiagnosticEngine& diags, bool normalize,
                                 bool loopStats) {
  sysy::sir::Arena arena;
  sysy::sir::Op* mod = sysy::sir::parseStructuredModule(src.text(), arena, diags);
  if (mod == nullptr) return std::string();   // 诊断已经报出；产物照常写出（可定位）
  if (normalize) {
    sysy::sir::LoopNormStats ls;
    mod = sysy::sir::normalizeLoops(mod, arena, ls);
    const size_t moved = sysy::sir::hoistAllocas(mod);
    if (loopStats) {
      std::cerr << sysy::sir::formatLoopNormStats(ls);
      std::cerr << "allocas-hoisted " << moved << "\n";
    }
    const std::vector<sysy::sir::Violation> vs = sysy::sir::verifyModule(mod);
    if (!vs.empty()) {
      diags.report(DiagLevel::Error, sysy::SourceLoc(0, 0), "E-STRUCT-VERIFY",
                   "读回后再规范化的结果违反不变量：\n" + sysy::sir::formatViolations(vs));
    }
  }
  return sysy::sir::dumpModule(mod);
}

// ============================================================================
// --emit=flat-ir 的真实实现（S06）
//   …（此处原有 11 行说明，已并入 S06 报告与设计文档）
// ============================================================================
std::string renderFlatIr(const SourceFile& src, DiagnosticEngine& diags, bool fromAst,
                         bool /*normalizeAlreadyImplied*/, bool flatStats) {
  std::unique_ptr<CompUnit> unit;
  if (fromAst) {
    unit = sysy::parseAstText(src.text(), diags);
  } else {
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    unit = parser.parseCompUnit();
  }
  if (unit == nullptr) return std::string();
  sysy::Sema sema(diags);
  sema.run(*unit);
  const sysy::InitPlan plan = sysy::runInitLowering(*unit, sema);
  sysy::sir::Arena arena;
  sysy::sir::Op* smod = sysy::sir::buildModule(arena, *unit, plan, baseNameOf(src.path()), diags);
  sysy::sir::LoopNormStats ls;
  smod = sysy::sir::normalizeLoops(smod, arena, ls);
  const size_t moved = sysy::sir::hoistAllocas(smod);
  const std::vector<sysy::sir::Violation> svs = sysy::sir::verifyModule(smod);
  if (!svs.empty()) {
    diags.report(DiagLevel::Error, sysy::SourceLoc(0, 0), "E-STRUCT-VERIFY",
                 "展平之前结构化 IR 已违反不变量：\n" + sysy::sir::formatViolations(svs));
  }
  // ★★ 平面校验只在"前面的阶段全绿"时才有意义 ★★
  //   前端已经报错时（张量语法、越界构造…），AST 是**带病恢复**出来的，
  //   结构化 IR 里已经出现"`load` 的操作数不是指针"这类形态 —— 再报一句
  //   "平面 IR 违反不变量（7 条）"是**假信号**：那些文件本来就不该有产物。
  //   实测：48 个张量文件共报 398 条 `[V6]`，全部落在**前端已拒绝**的文件里；
  //   490 个真正产出平面 IR 的文件**一条都不报**。
  const bool pipelineClean = !diags.hasError();
  sysy::flat::Module fmod;
  if (sysy::flat::flattenModule(smod, fmod, diags) == nullptr) return std::string();
  if (pipelineClean) {
    const std::vector<sysy::flat::Violation> fvs = sysy::flat::verifyFlatModule(fmod);
    if (!fvs.empty()) {
      // ★ S06 排障设施：`SYSY_DUMP_BAD_FLAT=1` 把**违约的那个平面模块**打到
      //   stderr。为什么必须留：违约报告只有"块号 + 行号"，而块号在文本 dump
      //   里才能对上（`L6` 到底有没有终结符、谁跳谁）—— 没有它就只能靠猜。
      //   ⚠️ 只在**违约束**里生效，正常路径不受影响（不改产物、不改 rc）。
      if (const char* want = std::getenv("SYSY_DUMP_BAD_FLAT"); want != nullptr && *want != '0') {
        std::cerr << "===== 违约模块的平面 IR =====\n" << sysy::flat::dumpModule(fmod) << "\n";
      }
      diags.report(DiagLevel::Error, sysy::SourceLoc(0, 0), "E-FLAT-VERIFY",
                   "平面 IR 违反不变量（" + std::to_string(fvs.size()) + " 条）：\n" +
                       sysy::flat::formatViolations(fvs));
    }
  }
  if (flatStats) {
    std::cerr << fmod.formatStats();
    std::cerr << "loopnorm rolled-back " << ls.rolledBack << " allocas-hoisted " << moved
              << "\n";
  }
  return sysy::flat::dumpModule(fmod);
}

// ============================================================================
// --from-flat 的真实实现（S06 轨 A：往返必须逐字节相同）
//   输入是 `--emit=flat-ir` 的产物；不经过 Lexer/Parser。读回 → 再 dump，
//   与原文逐字节相同（打印器与读取器互逆）。
// ============================================================================
std::string renderFromFlat(const SourceFile& src, DiagnosticEngine& diags, bool flatStats) {
  // `parseFlatModule` **新建**一个模块（所有权交给这里的 unique_ptr）
  std::unique_ptr<sysy::flat::Module> holder(sysy::flat::parseFlatModule(src.text(), diags));
  if (holder == nullptr) return std::string();   // 诊断已经报出；产物照常写出（可定位）
  sysy::flat::Module& fmod = *holder;
  const std::vector<sysy::flat::Violation> fvs = sysy::flat::verifyFlatModule(fmod);
  if (!fvs.empty()) {
    diags.report(DiagLevel::Error, sysy::SourceLoc(0, 0), "E-FLAT-VERIFY",
                 "读回的平面 IR 违反不变量：\n" + sysy::flat::formatViolations(fvs));
  }
  if (flatStats) std::cerr << fmod.formatStats();
  return sysy::flat::dumpModule(fmod);
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
        case EmitKind::StructuredIr:
          // ★ S05 的真实产物：结构化 IR（容器 → 文本）
          // ★ S05b：`--normalize` 在**同一棵树**上跑 LoopNormalize + AllocaHoist
          artifact = opts.fromStructured
                         ? renderFromStructured(src, diags, opts.normalize, opts.loopStats)
                         : renderStructuredIr(src, diags, opts.fromAst, opts.normalize,
                                              opts.loopStats);
          break;
        case EmitKind::FlatIr:
          // ★ S06 的真实产物：平面 IR（含 φ 与 critical-edge 拆分）
          artifact = opts.fromFlat
                         ? renderFromFlat(src, diags, opts.flatStats)
                         : renderFlatIr(src, diags, opts.fromAst, opts.normalize,
                                        opts.flatStats);
          break;
        case EmitKind::LlvmIr:
          // S07 才实现；本档是占位。
          artifact = sysy::mainstage::placeholderText(
              toString(opts.emit), src.path(), src.lineCount(), opts.optLevel >= 1,
              opts.structured);
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
