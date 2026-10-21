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

#include "main/CompilationSession.h"
#include "main/EmitText.h"

#include "support/Diagnostic.h"
#include "support/Errors.h"
#include "support/SourceFile.h"

#ifndef SYSY_COMPILER_VERSION
#define SYSY_COMPILER_VERSION "0.0.0-dev"
#endif

namespace {

using sysy::DiagnosticEngine;
using sysy::DiagLevel;
using sysy::IOError;
using sysy::SourceFile;
using sysy::SourceLoc;
using sysy::UsageError;
using sysy::mainstage::CompilationSession;

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
  int optLevel = 0;            // 默认 O0；仅 -O1/--optimize 启用优化
  bool toyBackend = false;     // --toy-backend：S11b 起表示"用自研降级器"；当前只接受
  bool fromAst = false;        // --from-ast：输入是 S-表达式（不是 SysY 源码）
  bool fromStructured = false;  // --from-structured：输入是结构化 IR 文本
  bool fromFlat = false;        // --from-flat：输入是平面 IR 文本（S06 轨 A）
  bool mem2reg = false;         // --mem2reg：显式开启 S09 标量提升
  bool s17 = false;             // --opt=s17：constprop + simplifycfg + dce
  bool s19 = false;             // --opt=s19：rewrite + gvn + dse + dle
  bool s20 = false;             // --opt=s20：licm + strength-reduce + dce
  bool s24 = false;             // --opt=s24：loop tiling
  bool s24b = false;            // --opt=s24b：blocked reduction/map
  bool s25 = false;             // --opt=s25：plan-sized loop unrolling
  bool flatStats = false;       // --dump-flat-stats：把平面 IR 统计打到 stderr
  bool optStats = false;        // --dump-opt-stats：把 s24/s25 pass counters 打到 stderr
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
                << " (front/middle end; F04 — Flat FunctionPass foundation)\n";
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
    // Deprecated compatibility flags: isolation is a runner-owned two-process
    // serialized Flat boundary, not a second AST->Flat pipeline.
    if (a == "--structured" || a == "--no-structured") { continue; }
    // --optimize 是 -O1 的别名（tools/run_tests.sh 用长选项名）。
    if (a == "--optimize")      { opts.optLevel = 1; continue; }
    // --toy-backend 由 tools/run_tests.sh 传入，用于让链路走【自研降级器】而非 clang。
    // 那是【测试链路】的选择，不是编译器的行为；编译器只需接受它（同 -S 的处理方式）。
    if (a == "--toy-backend")   { opts.toyBackend = true; continue; }
    if (a == "--from-ast")      { opts.fromAst = true; continue; }
    // --from-structured：输入是结构化 IR 文本（S05 轨 A 的"读回"路径）
    if (a == "--from-structured") { opts.fromStructured = true; continue; }
    // ★ S06：--from-flat（输入是平面 IR 文本）与 --dump-flat-stats（统计走 stderr）
    if (a == "--from-flat")     { opts.fromFlat = true; continue; }
    if (a == "--mem2reg")       { opts.mem2reg = true; continue; }
    if (a == "--opt=s17")      { opts.s17 = true; continue; }
    if (a == "--opt=s19")      { opts.s19 = true; continue; }
    if (a == "--opt=s20")      { opts.s20 = true; continue; }
    if (a == "--opt=s24")      { opts.s24 = true; continue; }
    if (a == "--opt=s24b")     { opts.s24b = true; continue; }
    if (a == "--opt=s25")      { opts.s25 = true; continue; }
    if (a == "--dump-flat-stats") { opts.flatStats = true; continue; }
    if (a == "--dump-opt-stats") { opts.optStats = true; continue; }
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

  if (opts.mem2reg && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--mem2reg requires --emit=flat-ir or --emit=llvm-ir");
  if (opts.s17 && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--opt=s17 requires --emit=flat-ir or --emit=llvm-ir");
  if (opts.s19 && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--opt=s19 requires --emit=flat-ir or --emit=llvm-ir");
  if (opts.s20 && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--opt=s20 requires --emit=flat-ir or --emit=llvm-ir");
  if (opts.s24 && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--opt=s24 requires --emit=flat-ir or --emit=llvm-ir");
  if (opts.s24b && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--opt=s24b requires --emit=flat-ir or --emit=llvm-ir");
  if (opts.s25 && opts.emit != EmitKind::FlatIr && opts.emit != EmitKind::LlvmIr)
    throw UsageError("--opt=s25 requires --emit=flat-ir or --emit=llvm-ir");

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
void ensureReadable(const SourceFile& src) {
  // 只做 I/O（读文件 + CRLF 规范化）。解析由 CompilationSession 负责。
  (void)src.text();
}

// ============================================================================
// --emit 分发：所有阶段物化都经 CompilationSession（单一物化路径）
// ============================================================================
std::string emitArtifact(CompilationSession& session, const Options& opts) {
  switch (opts.emit) {
    case EmitKind::Tokens:
      return session.tokensText();
    case EmitKind::Ast:
      return session.astText();
    case EmitKind::Sema:
      return session.semaText();
    case EmitKind::InitPlan:
      return session.initPlanText();
    case EmitKind::StructuredIr:
      return session.structuredText(opts.normalize, opts.loopStats);
    case EmitKind::FlatIr:
      return session.flatText(opts.fromFlat, opts.normalize, opts.flatStats,
                              opts.mem2reg, opts.s17, opts.s19, opts.s20, opts.s24, opts.s24b, opts.s25);
    case EmitKind::LlvmIr:
      return session.llvmIrText(opts.fromFlat, opts.normalize, opts.flatStats,
                                opts.mem2reg || opts.optLevel >= 1, opts.s17, opts.s19, opts.s20, opts.s24, opts.s24b, opts.s25);
    case EmitKind::Nothing:
      break;
  }
  return std::string();
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
      CompilationSession session(src, diags, opts.fromAst, opts.fromStructured);
      session.setOptStats(opts.optStats);
      const std::string artifact = emitArtifact(session, opts);
      // 文本 dump 即使伴随诊断仍可定位；LLVM 产物必须完整才写入。
      if (opts.emit == EmitKind::LlvmIr && diags.hasError()) {
        diags.printAll(std::cerr, false);
        return kCompileError;
      }
      // A failed promotion refuses to expose a partially rewritten Flat module.
      if (artifact.empty() && diags.hasError()) {
        diags.printAll(std::cerr, false);
        return kCompileError;
      }
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
