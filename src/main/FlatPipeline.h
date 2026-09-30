// Flat IR text pipeline helpers used by the command-line entry point.
#ifndef SYSY_MAIN_FLATPIPELINE_H
#define SYSY_MAIN_FLATPIPELINE_H

#include <string>

#include "ir/Module.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace sysy {
namespace mainstage {

bool applyMem2Reg(flat::Module& module, DiagnosticEngine& diags, bool enabled,
                  bool reportStats);
std::string renderFromFlat(const SourceFile& src, DiagnosticEngine& diags,
                           bool flatStats, bool mem2reg);

}  // namespace mainstage
}  // namespace sysy

#endif  // SYSY_MAIN_FLATPIPELINE_H
