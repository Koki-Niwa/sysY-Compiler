// F05: one materialization path per stage; formatting stays artifact-local.
#include "main/CompilationSession.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "frontend/AstPrinter.h"
#include "frontend/InitLowering.h"
#include "frontend/InitLoweringDump.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "frontend/SemaDump.h"
#include "frontend/Token.h"
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "ir/LLVMTextEmitter.h"
#include "main/FlatPipeline.h"
#include "opt/ConstantPropagation.h"
#include "opt/DeadCodeElimination.h"
#include "opt/FlatPassManager.h"
#include "opt/SimplifyCFG.h"
#include "opt/RewriteEngine.h"
#include "opt/GVN.h"
#include "opt/DeadStoreElimination.h"
#include "opt/DeadLoadElimination.h"
#include "opt/LICM.h"
#include "opt/StrengthReduce.h"
#include "opt/LoopTiling.h"
#include "opt/LoopBlocking.h"
#include "structured/AllocaHoist.h"
#include "structured/FlattenCFG.h"
#include "structured/IRGen.h"
#include "structured/LoopNormalize.h"
#include "structured/StructuredDump.h"
#include "structured/StructuredReader.h"
#include "structured/StructuredVerifier.h"

namespace sysy {
namespace mainstage {
namespace {

std::string baseNameOf(const std::string& path) {
  const size_t p = path.find_last_of("/\\");
  return (p == std::string::npos) ? path : path.substr(p + 1);
}

bool runS20Pipeline(flat::Module& module, DiagnosticEngine& diags) {
  flat::FlatPassManager manager;
  manager.addPass(std::make_unique<flat::LICMPass>());
  manager.addPass(std::make_unique<flat::StrengthReducePass>());
  manager.addPass(std::make_unique<flat::DeadCodeEliminationPass>());
  const flat::PipelineResult result = manager.run(module);
  if (!result.ok()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S20-OPT", result.diagnostic);
    return false;
  }
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(module);
  if (!violations.empty()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S20-VERIFY",
                 "S20 输出违反平面 IR 不变量：\n" + flat::formatViolations(violations));
    return false;
  }
  return true;
}

bool runS24Pipeline(flat::Module& module, DiagnosticEngine& diags) {
  flat::FlatPassManager manager;
  manager.addPass(std::make_unique<flat::LoopTilingPass>());
  manager.addPass(std::make_unique<flat::LoopBlockingPass>());
  const flat::PipelineResult result = manager.run(module);
  if (!result.ok()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S24-OPT", result.diagnostic);
    return false;
  }
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(module);
  if (!violations.empty()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S24-VERIFY",
                 "S24 输出违反平面 IR 不变量：\n" + flat::formatViolations(violations));
    return false;
  }
  return true;
}

bool runS19Pipeline(flat::Module& module, DiagnosticEngine& diags) {
  flat::FlatPassManager manager;
  manager.addPass(std::make_unique<flat::RewritePass>());
  manager.addPass(std::make_unique<flat::GVNPass>());
  manager.addPass(std::make_unique<flat::DeadStoreEliminationPass>());
  manager.addPass(std::make_unique<flat::DeadLoadEliminationPass>());
  const flat::PipelineResult result = manager.run(module);
  if (!result.ok()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S19-OPT", result.diagnostic);
    return false;
  }
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(module);
  if (!violations.empty()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S19-VERIFY",
                 "S19 输出违反平面 IR 不变量：\n" + flat::formatViolations(violations));
    return false;
  }
  return true;
}

bool runS17Pipeline(flat::Module& module, DiagnosticEngine& diags) {
  flat::FlatPassManager manager;
  manager.addPass(std::make_unique<flat::ConstantPropagationPass>());
  manager.addPass(std::make_unique<flat::SimplifyCFGPass>());
  manager.addPass(std::make_unique<flat::DeadCodeEliminationPass>());
  const flat::PipelineResult result = manager.run(module);
  if (!result.ok()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S17-OPT", result.diagnostic);
    return false;
  }
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(module);
  if (!violations.empty()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-S17-VERIFY",
                 "S17 输出违反平面 IR 不变量：\n" + flat::formatViolations(violations));
    return false;
  }
  return true;
}

void reportMem2RegStats(const flat::Mem2RegResult& result) {
  std::cerr << "mem2reg promoted " << result.promotedAllocas
            << " allocas generated-phis " << result.generatedPhis
            << " eliminated-phis " << result.eliminatedPhis << "\n";
}

}  // namespace

CompilationSession::CompilationSession(const SourceFile& source,
                                       DiagnosticEngine& diags, bool fromAst,
                                       bool fromStructured)
    : source_(source),
      diags_(diags),
      fromAst_(fromAst),
      fromStructured_(fromStructured) {}

CompilationSession::~CompilationSession() = default;

bool CompilationSession::materializeAst() {
  if (unit_ != nullptr) return true;
  if (fromAst_) {
    // S-表达式输入：不经过 Lexer。SourceFile 的生命期覆盖整个会话，因此
    // 字面量节点里指向 source_.text() 的 string_view 一直有效。
    unit_ = parseAstText(source_.text(), diags_);
  } else {
    Lexer lexer(source_, diags_);
    Parser parser(lexer, diags_);
    unit_ = parser.parseCompUnit();
  }
  return unit_ != nullptr;
}

bool CompilationSession::materializeSema() {
  if (sema_ != nullptr) return true;
  if (!materializeAst()) return false;
  sema_ = std::make_unique<Sema>(diags_);
  sema_->run(*unit_);
  return true;
}

bool CompilationSession::materializeInitPlan() {
  if (plan_ != nullptr) return true;
  if (!materializeSema()) return false;
  // Sema 的符号层在 run() 结束后仍然可查，runInitLowering 依赖这一点。
  plan_ = std::make_unique<InitPlan>(runInitLowering(*unit_, *sema_));
  return true;
}

std::string CompilationSession::tokens() {
  Lexer lexer(source_, diags_);
  const std::vector<Token> tokens = lexer.tokenize();
  std::string out;
  out.reserve(tokens.size() * 24 + 16);
  char buf[32];
  for (const Token& tok : tokens) {
    std::snprintf(buf, sizeof(buf), "%u", tok.loc.line);
    out += buf;
    out += '\t';
    std::snprintf(buf, sizeof(buf), "%u", tok.loc.col);
    out += buf;
    out += '\t';
    out += tokKindDumpName(tok.kind);
    out += '\t';
    if (tok.kind == TokKind::EndOfFile) {
      out += '-';
    } else {
      const std::string_view text = tokenText(tok, source_);
      out.append(text.data(), text.size());
    }
    out += '\n';
  }
  return out;
}

const CompUnit* CompilationSession::ast() {
  return materializeAst() ? unit_.get() : nullptr;
}

const Sema* CompilationSession::sema() {
  return materializeSema() ? sema_.get() : nullptr;
}

const InitPlan* CompilationSession::initPlan() {
  return materializeInitPlan() ? plan_.get() : nullptr;
}

const sir::Op* CompilationSession::structured(bool normalize, bool loopStats) {
  if (structured_.ready && structured_.normalize == normalize &&
      structured_.loopStats == loopStats)
    return structured_.module;
  structured_.ready = true;
  structured_.normalize = normalize;
  structured_.loopStats = loopStats;
  structured_.module = nullptr;
  structured_.statsText.clear();
  sir::Op* mod = nullptr;
  if (fromStructured_) {
    // S-表达式输入：不经过 AST/Sema；读回失败时诊断已报，返回 nullptr。
    mod = sir::parseStructuredModule(source_.text(), structured_.arena, diags_);
    if (mod == nullptr) return nullptr;
  } else {
    if (!materializeInitPlan()) return nullptr;
    // Sema 的生命期必须覆盖 IRGen（IRGen 要用 Cast/类型注解，但不再查符号表）。
    mod = sir::buildModule(structured_.arena, *unit_, *plan_,
                           baseNameOf(source_.path()), diags_);
  }
  if (normalize) {
    // ① 循环规范化只对满足成功条件的 while 生效；② alloca 提升每个函数都做。
    sir::LoopNormStats stats;
    mod = sir::normalizeLoops(mod, structured_.arena, stats);
    structured_.rolledBack = stats.rolledBack;
    structured_.allocasHoisted = sir::hoistAllocas(mod);
    structured_.statsText = sir::formatLoopNormStats(stats);
    structured_.statsText += "rolled-back " + std::to_string(structured_.rolledBack) + "\n";
    structured_.statsText += "allocas-hoisted " + std::to_string(structured_.allocasHoisted) + "\n";
  }
  // from-structured only verifies after normalization (the S05 read-back
  // contract); the source path always verifies the freshly generated IR.
  const std::vector<sir::Violation> violations =
      (normalize || !fromStructured_) ? sir::verifyModule(mod)
                                      : std::vector<sir::Violation>{};
  if (!violations.empty()) {
    std::string msg = "结构化 IR 违反不变量（" + std::to_string(violations.size()) + " 条）：\n";
    for (size_t i = 0; i < violations.size() && i < 10; ++i) {
      msg += "  [";
      msg += violations[i].invariant;
      msg += "] ";
      if (violations[i].loc.line != 0) msg += "line " + std::to_string(violations[i].loc.line) + ": ";
      msg += violations[i].message;
      msg += '\n';
    }
    diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-STRUCT-VERIFY", msg);
  }
  structured_.module = mod;
  return mod;
}

std::string CompilationSession::structuredText(bool normalize, bool loopStats) {
  const sir::Op* mod = structured(normalize, loopStats);
  if (mod == nullptr) return std::string();
  // 统计绝不进 dump（dump 是与 S05 样例对的冻结契约），走 stderr。
  if (loopStats && normalize) std::cerr << structured_.statsText;
  return sir::dumpModule(mod);
}

const flat::Module* CompilationSession::flat(bool fromFlat, bool normalize,
                                             bool flatStats, bool mem2reg,
                                             bool s17, bool s19, bool s20,
                                             bool s24) {
  if (flat_.ready && flat_.fromFlat == fromFlat && flat_.normalize == normalize &&
      flat_.flatStats == flatStats && flat_.mem2reg == mem2reg &&
      flat_.s17 == s17 && flat_.s19 == s19 && flat_.s20 == s20 && flat_.s24 == s24)
    return flat_.module.get();
  flat_.ready = true;
  flat_.fromFlat = fromFlat;
  flat_.normalize = normalize;
  flat_.flatStats = flatStats;
  flat_.mem2reg = mem2reg;
  flat_.s17 = s17;
  flat_.s19 = s19;
  flat_.s20 = s20;
  flat_.s24 = s24;
  flat_.module.reset();
  flat_.mem2RegResult = flat::Mem2RegResult{};

  if (fromFlat) {
    // Serialized Flat boundary: a second process must not rebuild the AST.
    std::unique_ptr<flat::Module> holder(flat::parseFlatModule(source_.text(), diags_));
    if (!holder) return nullptr;
    const std::vector<flat::Violation> violations = flat::verifyFlatModule(*holder);
    if (!violations.empty()) {
      diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-FLAT-VERIFY",
                    "读回的平面 IR 违反不变量：\n" + flat::formatViolations(violations));
    }
    if (!diags_.hasError()) {
      flat_.mem2RegResult = flat::runMem2Reg(*holder, flat::Mem2RegOptions{mem2reg, false});
      if (!flat_.mem2RegResult.success) {
        diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-MEM2REG",
                      flat_.mem2RegResult.diagnostic);
        return nullptr;
      }
      const std::vector<flat::Violation> post = flat::verifyFlatModule(*holder);
      if (!post.empty()) {
        diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-MEM2REG-VERIFY",
                      "mem2reg 输出违反平面 IR 不变量：\n" + flat::formatViolations(post));
        return nullptr;
      }
      if (flatStats) reportMem2RegStats(flat_.mem2RegResult);
      if (s17 && !runS17Pipeline(*holder, diags_)) return nullptr;
      if (s19 && !runS19Pipeline(*holder, diags_)) return nullptr;
      if (s20 && !runS20Pipeline(*holder, diags_)) return nullptr;
      if (s24 && !runS24Pipeline(*holder, diags_)) return nullptr;
    }
    flat_.module = std::move(holder);
    return flat_.module.get();
  }

  // The flat product path always normalizes and hoists before flattening;
  // `--normalize` only controls the structured dump mode.
  const sir::Op* mod = structured(true, false);
  if (mod == nullptr) return nullptr;
  const std::vector<sir::Violation> svs = sir::verifyModule(mod);
  if (!svs.empty()) {
    diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-STRUCT-VERIFY",
                  "展平之前结构化 IR 已违反不变量：\n" + sir::formatViolations(svs));
  }
  // Recovery IR after a frontend error can violate downstream invariants too;
  // skip flat verification there to avoid misleading secondary diagnostics.
  const bool pipelineClean = !diags_.hasError();
  auto fmod = std::make_unique<flat::Module>();
  if (flat::flattenModule(mod, *fmod, diags_) == nullptr) return nullptr;
  if (pipelineClean) {
    const std::vector<flat::Violation> fvs = flat::verifyFlatModule(*fmod);
    if (!fvs.empty()) {
      if (const char* want = std::getenv("SYSY_DUMP_BAD_FLAT"); want != nullptr && *want != '0') {
        std::cerr << "===== 违约模块的平面 IR =====\n" << flat::dumpModule(*fmod) << "\n";
      }
      diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-FLAT-VERIFY",
                    "平面 IR 违反不变量（" + std::to_string(fvs.size()) + " 条）：\n" +
                        flat::formatViolations(fvs));
    }
  }
  if (pipelineClean && mem2reg) {
    // Same one-wrapper contract as pre-F05: run, then one output verification.
    flat_.mem2RegResult = flat::runMem2Reg(*fmod, flat::Mem2RegOptions{true, flatStats});
    if (!flat_.mem2RegResult.success) {
      diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-MEM2REG",
                    flat_.mem2RegResult.diagnostic);
      return nullptr;
    }
    const std::vector<flat::Violation> post = flat::verifyFlatModule(*fmod);
    if (!post.empty()) {
      diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-MEM2REG-VERIFY",
                    "mem2reg 输出违反平面 IR 不变量：\n" + flat::formatViolations(post));
      return nullptr;
    }
    if (flatStats) reportMem2RegStats(flat_.mem2RegResult);
  }
  if (pipelineClean && s17 && !runS17Pipeline(*fmod, diags_)) return nullptr;
  if (pipelineClean && s19 && !runS19Pipeline(*fmod, diags_)) return nullptr;
  if (pipelineClean && s20 && !runS20Pipeline(*fmod, diags_)) return nullptr;
  if (pipelineClean && s24 && !runS24Pipeline(*fmod, diags_)) return nullptr;
  flat_.module = std::move(fmod);
  return flat_.module.get();
}

std::string CompilationSession::tokensText() { return tokens(); }

std::string CompilationSession::astText() {
  const CompUnit* unit = ast();
  return unit == nullptr ? std::string() : printAst(*unit);
}

std::string CompilationSession::semaText() {
  const Sema* sema = this->sema();
  return sema == nullptr ? std::string() : printSemaDump(*unit_);
}

std::string CompilationSession::initPlanText() {
  const InitPlan* plan = initPlan();
  return plan == nullptr ? std::string() : printInitPlanDump(*plan);
}

std::string CompilationSession::flatText(bool fromFlat, bool normalize,
                                         bool flatStats, bool mem2reg, bool s17, bool s19, bool s20, bool s24) {
  const flat::Module* module = flat(fromFlat, normalize, flatStats, mem2reg, s17, s19, s20, s24);
  if (module == nullptr) return std::string();
  if (flatStats) {
    std::cerr << module->formatStats();
    if (!fromFlat) {
      std::cerr << "loopnorm rolled-back " << structured_.rolledBack
                << " allocas-hoisted " << structured_.allocasHoisted << "\n";
    }
  }
  return flat::dumpModule(*module);
}

std::string CompilationSession::llvmIrText(bool fromFlat, bool normalize,
                                           bool flatStats, bool mem2reg, bool s17, bool s19, bool s20, bool s24) {
  const flat::Module* module = flat(fromFlat, normalize, flatStats, mem2reg, s17, s19, s20, s24);
  if (module == nullptr || diags_.hasError()) return std::string();
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(*module);
  if (!violations.empty()) {
    diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-FLAT-VERIFY",
                  "发射 LLVM 前平面 IR 违反不变量：\n" +
                      flat::formatViolations(violations));
    return std::string();
  }
  flat::LLVMTextEmitter emitter;
  std::string output;
  std::string error;
  if (!emitter.emit(*module, output, error)) {
    diags_.report(DiagLevel::Error, SourceLoc(0, 0), "E-LLVM-EMIT", error);
    return std::string();
  }
  return output;
}

}  // namespace mainstage
}  // namespace sysy
