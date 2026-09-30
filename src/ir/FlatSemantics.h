// The dependency leaf for flat instruction shape and type legality.
#ifndef SYSY_IR_FLATSEMANTICS_H
#define SYSY_IR_FLATSEMANTICS_H

#include <string>

#include "ir/Instruction.h"

namespace sysy {
namespace flat {

// Validate the flat IR contract that is local to one instruction.  CFG
// membership, function signatures, and LLVM spelling remain consumer checks.
// A false result always has a short diagnostic in `error`.
bool validateFlatInstruction(const Instruction& instruction, std::string& error);

}  // namespace flat
}  // namespace sysy

#endif  // SYSY_IR_FLATSEMANTICS_H
