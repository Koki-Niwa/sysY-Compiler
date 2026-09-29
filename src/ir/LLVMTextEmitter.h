// LLVM's target-independent textual dialect for the supported flat IR subset.
#ifndef SYSY_IR_LLVM_TEXT_EMITTER_H
#define SYSY_IR_LLVM_TEXT_EMITTER_H

#include "ir/IREmitter.h"

namespace sysy {
namespace flat {

class LLVMTextEmitter final : public IREmitter {
 public:
  // Input: a verified flat module. Output is committed only on full success.
  // Failure: returns false and sets a stable, user-readable explanation.
  bool emit(const Module& module, std::string& output,
            std::string& error) const override;
};

}  // namespace flat
}  // namespace sysy

#endif  // SYSY_IR_LLVM_TEXT_EMITTER_H
