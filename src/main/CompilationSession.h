// ============================================================================
// main/CompilationSession.h —— 一条物化路径 + 阶段缓存（F05）
//
// 为什么需要它：F05 之前 main.cpp 里 renderAst / renderSema / renderInitPlan /
// renderStructuredIr / renderFlatIr 各自重写了「读源文件 → 词法 → 语法 →
// Sema → InitLowering」，同一输入有 4 份物化路径。任何一处格式或诊断差异
// 都会变成"同一阶段两种行为"。
//
// 本类只负责**按需物化**并缓存结果，不解析命令行、不写产物：
//   AST → Sema → InitPlan → Structured → Flat → LLVM 文本
// 每个阶段最多物化一次；调用方（main）只决定 --emit 分支与产物策略。
//
// 生命周期：SourceFile 与 DiagnosticEngine 由 main 持有并覆盖整个会话；
// 本类只保存引用。物化失败（诊断已报）时返回 nullptr / 空串，调用方按
// 既有产物策略处理。
// ============================================================================
#ifndef SYSY_MAIN_COMPILATIONSESSION_H
#define SYSY_MAIN_COMPILATIONSESSION_H

#include <memory>
#include <string>

#include "frontend/Ast.h"
#include "frontend/InitPlan.h"
#include "ir/Module.h"
#include "opt/Mem2Reg.h"
#include "structured/StructuredIR.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace sysy {

class ConstEnv;
class Sema;

namespace mainstage {

class CompilationSession {
 public:
  CompilationSession(const SourceFile& source, DiagnosticEngine& diags,
                     bool fromAst = false, bool fromStructured = false);
  ~CompilationSession();

  // Raw artifacts. Each stage is materialized at most once.
  std::string tokens();
  const CompUnit* ast();
  const Sema* sema();
  const InitPlan* initPlan();
  void setOptStats(bool enabled) { optStats_ = enabled; }

  // Structured IR. `normalize` maps to --normalize; the result is cached by
  // the flag pair so the two documented modes never share state.
  const sir::Op* structured(bool normalize, bool loopStats);

  // Flat IR. The product path always normalizes, hoists allocas and verifies
  // before flattening; `mem2reg` then runs through the F04 wrapper.
  const flat::Module* flat(bool fromFlat, bool normalize, bool flatStats,
                           bool mem2reg, bool s17, bool s19, bool s20, bool s24, bool s24b, bool s25);

  // Text artifacts. These are the only functions that own dump formatting.
  std::string tokensText();
  std::string astText();
  std::string semaText();
  std::string initPlanText();
  std::string structuredText(bool normalize, bool loopStats);
  std::string flatText(bool fromFlat, bool normalize, bool flatStats,
                       bool mem2reg, bool s17, bool s19, bool s20, bool s24, bool s24b, bool s25);
  std::string llvmIrText(bool fromFlat, bool normalize, bool flatStats,
                         bool mem2reg, bool s17, bool s19, bool s20, bool s24, bool s24b, bool s25);

 private:
  bool materializeAst();
  bool materializeSema();
  bool materializeInitPlan();

  const SourceFile& source_;
  DiagnosticEngine& diags_;
  const bool fromAst_;
  const bool fromStructured_;

  std::unique_ptr<CompUnit> unit_;
  std::unique_ptr<Sema> sema_;
  std::unique_ptr<InitPlan> plan_;

  struct StructuredCache {
    bool ready = false;
    bool normalize = false;
    bool loopStats = false;
    size_t rolledBack = 0;
    size_t allocasHoisted = 0;
    sir::Arena arena;
    sir::Op* module = nullptr;
    std::string statsText;
  } structured_;

  struct FlatCache {
    bool ready = false;
    bool fromFlat = false;
    bool normalize = false;
    bool flatStats = false;
    bool mem2reg = false;
    bool s17 = false;
    bool s19 = false;
    bool s20 = false;
    bool s24 = false;
    bool s24b = false;
    bool s25 = false;
    flat::Mem2RegResult mem2RegResult;
    std::unique_ptr<flat::Module> module;
  } flat_;
  bool optStats_ = false;
};

}  // namespace mainstage
}  // namespace sysy

#endif  // SYSY_MAIN_COMPILATIONSESSION_H
