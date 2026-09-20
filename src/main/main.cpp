// ============================================================================
// main —— 命令行入口（P00 prompt §2）
//
//   compiler <input> -o <output> [选项]
//     -o <file>          输出文件（必填）
//     --emit=<kind>      llvm-ir（默认）| tokens | ast | structured-ir | nothing
//     -O1                开启性能优化（S00 只记录，不实现）
//     -S                 比赛调用形式，必须接受；本阶段忽略其含义
//     -h, --help / -v, --version
//
// 【S00 的成功标准】解析命令行 → 读文件 → 按 --emit 输出。
// llvm-ir 暂时只输出占位注释（S05 起才有真 IR）。前端一行都还没有。
//
// 铁律 4：编译器本体不调用任何外部程序（这里没有 system/exec/fork）。
// ============================================================================
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

// 退出码约定（run_tests.sh 依赖 0/非 0 区分；1 专指"编译错误"）
enum ExitCode {
  kOk = 0,
  kCompileError = 1,
  kUsageError = 2,
  kIOError = 3,
  kInterrupted = 130,
};

enum class EmitKind { LlvmIr, Tokens, Ast, StructuredIr, Nothing };

const char* toString(EmitKind k) {
  switch (k) {
    case EmitKind::LlvmIr:       return "llvm-ir";
    case EmitKind::Tokens:       return "tokens";
    case EmitKind::Ast:          return "ast";
    case EmitKind::StructuredIr: return "structured-ir";
    case EmitKind::Nothing:      return "nothing";
  }
  return "llvm-ir";
}

bool parseEmitKind(const std::string& s, EmitKind& out) {
  if (s == "llvm-ir")       { out = EmitKind::LlvmIr;       return true; }
  if (s == "tokens")        { out = EmitKind::Tokens;       return true; }
  if (s == "ast")           { out = EmitKind::Ast;          return true; }
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
  bool verbose = false;
};

const char* kHelpText =
    "SysY compiler (front/middle end) —— SysY -> LLVM IR subset\n"
    "\n"
    "用法: compiler <input> -o <output> [选项]\n"
    "\n"
    "选项:\n"
    "  -o <file>          输出文件（必填）\n"
    "  --emit=<kind>      产物类型: llvm-ir（默认）| tokens | ast | structured-ir | nothing\n"
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
                << " (front/middle end; stage S00)\n";
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
                         "' (expected: llvm-ir | tokens | ast | structured-ir | nothing)");
      continue;
    }
    if (a.rfind("--emit=", 0) == 0) {
      const std::string v = a.substr(7);
      if (!parseEmitKind(v, opts.emit))
        throw UsageError("unknown --emit kind '" + v +
                         "' (expected: llvm-ir | tokens | ast | structured-ir | nothing)");
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

// —— 各 emit 模式的本阶段产物（全部是占位，不含任何真实编译逻辑）——
std::string renderPlaceholder(EmitKind kind, const Options& opts, const SourceFile& src) {
  std::string out;
  out += "; SysY compiler (front/middle end) —— stage S00 skeleton\n";
  out += "; input:  " + src.path() + "\n";
  out += "; emit:   " + std::string(toString(kind)) + "\n";
  out += "; lines:  " + std::to_string(src.lineCount()) + "\n";
  out += std::string("; -O1:    ") +
         (opts.optLevel >= 1 ? "requested (not implemented yet)" : "off") + "\n";
  out += std::string("; structured IR layer: ") + (opts.structured ? "on" : "off") + "\n";
  out += ";\n";
  out += "; 本阶段不实现任何前端/IR（见 phases/P00-skeleton.md §七）。\n";
  out += "; 交给后端的 .ll 契约（TESTING-GUIDE §3.3）：目标无关 ——\n";
  out += ";   不带 target triple、不带 target datalayout、\n";
  out += ";   函数属性里不带 target-cpu / target-features。\n";
  out += "; 目标由降级阶段（llc -mtriple / 自研后端）决定。\n";
  return out;
}

void ensureReadable(const SourceFile& src) {
  // 只做 I/O（读文件 + CRLF 规范化）。不解析、不分析 —— 那是 S01 之后的事。
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
        case EmitKind::LlvmIr:
        case EmitKind::Tokens:
        case EmitKind::Ast:
        case EmitKind::StructuredIr:
          artifact = renderPlaceholder(opts.emit, opts, src);
          break;
        case EmitKind::Nothing:
          break;
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
