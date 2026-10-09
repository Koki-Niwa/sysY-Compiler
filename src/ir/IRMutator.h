#ifndef SYSY_IR_IRMUTATOR_H
#define SYSY_IR_IRMUTATOR_H
#include <initializer_list>
#include <vector>
#include "ir/Module.h"

namespace sysy { namespace flat {
class CFGEditor;

// The only tracked non-CFG live mutation authority. Raw construction and
// reader repair methods never advance revision facts.
class IRMutator {
 public:
  explicit IRMutator(Module& module) : module_(module) {}
  Module& module() const { return module_; }
  const MutationSummary& summary() const { return summary_; }

  MutationResult setOperand(Instruction* user, size_t index, Value* replacement);
  MutationResult replaceAllUsesWith(Value* oldValue, Value* replacement);
  MutationResult insertInstruction(BasicBlock* block, size_t index, Instruction* inst);
  MutationResult eraseInstruction(Instruction* inst);
  MutationResult replaceAndErase(Instruction* oldInst, Value* replacement);

  // Scratch planning cleanup, only for unowned detached instructions. Does not
  // modify finalized live IR or revisions. Clears all uses before any delete.
  MutationResult discardDetached(const std::vector<Instruction*>& scratch);

 private:
  friend class CFGEditor;
  Module& module_;
  MutationSummary summary_;
  bool isLiveBlock(const BasicBlock* block) const;
  bool isLive(const Instruction* inst) const;
  bool operandLegal(const Instruction* user, size_t index, Value* value) const;
  bool validUses(const Value* value) const;
  bool valueAvailable(Value* value, bool forLive) const;
  bool rawErase(Instruction* inst);
  void commit(MutationKind kinds, const std::vector<Function*>& functions);
  static MutationKind kindsFor(Opcode op);
};
}} // namespace sysy::flat
#endif
