// Flat IR invariant checking. Structural checks precede all indexed CFG and
// SSA analysis so malformed public input is reported instead of dereferenced.
#include "ir/FlatVerifier.h"

#include "ir/FlatSemantics.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace sysy {
namespace flat {
namespace {

bool isRealOpcode(Opcode op) {
  return op != Opcode::Nop && op != Opcode::ConstantInt && op != Opcode::ConstantFP &&
         op != Opcode::kCount;
}

template <typename T>
size_t pointerCount(const std::vector<T*>& values, const T* needle) {
  return static_cast<size_t>(std::count(values.begin(), values.end(), needle));
}

bool definedBefore(const Instruction* def, const Instruction* use) {
  if (def == nullptr || use == nullptr || def == use || def->parent() == nullptr ||
      def->parent() != use->parent()) {
    return false;
  }
  bool sawDef = false;
  for (Instruction* in : def->parent()->insts()) {
    if (in == def) sawDef = true;
    if (in == use) return sawDef;
  }
  return false;
}

std::string kindName(Opcode op) {
  if (op == Opcode::ConstantInt || op == Opcode::ConstantFP) return "常量";
  return opcodeName(op);
}

}  // namespace

std::vector<Violation> verifyFlatModule(const Module& m) {
  std::vector<Violation> bad;
  auto add = [&](const char* invariant, const std::string& function, SourceLoc loc,
                 const std::string& message) {
    bad.push_back(Violation{invariant, loc, message, function});
  };
  std::unordered_set<GlobalVariable*> moduleGlobals;
  for (GlobalVariable* global : m.globals()) {
    if (global != nullptr) moduleGlobals.insert(global);
  }

  for (size_t fi = 0; fi < m.numFunctions(); ++fi) {
    Function* f = m.function(fi);
    if (f == nullptr) {
      add("V4", std::string(), SourceLoc(), "模块函数表含有空函数");
      continue;
    }
    if (f->isDeclaration()) continue;
    if (f->blockCount() == 0) {
      add("V4", f->name(), SourceLoc(), "非声明函数没有基本块");
      continue;
    }
    const std::string& fname = f->name();

    // The vector and ownership checks are intentionally independent from
    // rebuildCFG. A stale cache is a public counterexample, and a caller can
    // also mutate block/index/parent fields before verification.
    std::vector<BasicBlock*> blocks;
    std::unordered_set<BasicBlock*> blockSet;
    std::unordered_set<Instruction*> instructionSet;
    std::unordered_map<BasicBlock*, std::unordered_set<BasicBlock*>> expectedPreds;
    std::unordered_set<Instruction*> validTerminators;
    bool structurallySafe = true;

    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* block = f->at(bi);
      if (block == nullptr) {
        add("V4", fname, SourceLoc(), "函数块表含有空块");
        structurallySafe = false;
        continue;
      }
      if (!blockSet.insert(block).second) {
        add("V4", fname, SourceLoc(), "函数块表重复登记同一个基本块");
        structurallySafe = false;
      }
      if (block->parent() != f) {
        add("V4", fname, SourceLoc(), "基本块归属函数与块表不一致");
        structurallySafe = false;
      }
      if (block->index() != bi) {
        add("V4", fname, SourceLoc(),
            "基本块下标不密集或与块表位置不一致：L" + std::to_string(block->index()) +
                " 应为 L" + std::to_string(bi));
        structurallySafe = false;
      }
      blocks.push_back(block);
    }

    for (BasicBlock* block : blocks) {
      size_t terminatorCount = 0;
      Instruction* lastTerminator = nullptr;
      for (size_t ii = 0; ii < block->size(); ++ii) {
        Instruction* instruction = block->at(ii);
        if (instruction == nullptr) {
          add("V1", fname, SourceLoc(), "基本块指令表含有空指令");
          structurallySafe = false;
          continue;
        }
        if (!instructionSet.insert(instruction).second) {
          add("V1", fname, instruction->loc(), "同一条指令重复出现在多个基本块中");
          structurallySafe = false;
        }
        if (instruction->parent() != block) {
          add("V1", fname, instruction->loc(), "指令归属基本块与指令表不一致");
          structurallySafe = false;
        }
        if (!isRealOpcode(instruction->op())) {
          add("V6", fname, instruction->loc(),
              "出现了非指令的 opcode：" + kindName(instruction->op()) +
                  "（容器占位不该被发射）");
        } else {
          std::string error;
          if (!validateFlatInstruction(*instruction, error)) {
            bool nullOperand = false;
            for (Value* operand : instruction->operands()) {
              if (operand == nullptr || operand->type() == nullptr) {
                nullOperand = true;
                break;
              }
            }
            add(nullOperand ? "V1" : (isTerminatorOpcode(instruction->op()) ? "V4" : "V6"),
                fname, instruction->loc(), error);
            structurallySafe = false;
          }
        }
        for (BasicBlock* successor : instruction->succs()) {
          if (successor == nullptr || successor->parent() != f ||
              blockSet.count(successor) == 0) {
            add("V4", fname, instruction->loc(), "后继块为空或不属于该函数的块表");
            structurallySafe = false;
          }
        }
        if (isTerminatorOpcode(instruction->op())) {
          ++terminatorCount;
          lastTerminator = instruction;
          if (ii + 1 != block->size()) {
            add("V4", fname, instruction->loc(),
                std::string("终结符不是最后一条：") + opcodeName(instruction->op()) +
                    " 后面还有指令");
            structurallySafe = false;
          }
        }
      }
      if (terminatorCount == 0) {
        add("V4", fname, SourceLoc(), "块没有终结符（每个块恰好一个）");
        structurallySafe = false;
      } else if (terminatorCount != 1) {
        add("V4", fname, lastTerminator->loc(),
            "块有 " + std::to_string(terminatorCount) + " 个终结符");
        structurallySafe = false;
      }
      if (terminatorCount == 1 && lastTerminator == block->back()) {
        std::string error;
        if (validateFlatInstruction(*lastTerminator, error)) {
          validTerminators.insert(lastTerminator);
        } else {
          structurallySafe = false;
        }
      }
    }

    // Recompute predecessor membership from validated terminators. A source
    // with two identical successor entries contributes one predecessor.
    for (BasicBlock* block : blocks) {
      Instruction* terminator = block->back();
      if (terminator == nullptr || validTerminators.count(terminator) == 0) continue;
      for (size_t si = 0; si < terminator->numSuccs(); ++si) {
        BasicBlock* successor = terminator->succ(si);
        if (successor == nullptr) {
          add("V4", fname, terminator->loc(), "分支目标为空");
          structurallySafe = false;
          continue;
        }
        if (successor->parent() != f || blockSet.count(successor) == 0) {
          add("V4", fname, terminator->loc(), "分支目标不属于该函数的块表");
          structurallySafe = false;
          continue;
        }
        expectedPreds[successor].insert(block);
      }
    }
    for (BasicBlock* block : blocks) {
      std::unordered_set<BasicBlock*> cached;
      bool hasPhi = false;
      for (Instruction* instruction : block->insts()) {
        if (instruction != nullptr && instruction->op() == Opcode::Phi) {
          hasPhi = true;
          break;
        }
      }
      const char* cacheInvariant = hasPhi ? "V3" : "V4";
      for (BasicBlock* predecessor : block->preds()) {
        if (predecessor == nullptr || predecessor->parent() != f ||
            blockSet.count(predecessor) == 0) {
          add(cacheInvariant, fname, SourceLoc(), "前驱缓存含有空项或函数外基本块");
          structurallySafe = false;
          continue;
        }
        if (!cached.insert(predecessor).second) {
          add(cacheInvariant, fname, SourceLoc(), "前驱缓存含有重复来源");
          structurallySafe = false;
        }
      }
      for (size_t pi = 1; pi < block->numPreds(); ++pi) {
        BasicBlock* previous = block->pred(pi - 1);
        BasicBlock* current = block->pred(pi);
        if (previous == nullptr || current == nullptr) continue;
        if (previous->index() >= current->index()) {
          add(cacheInvariant, fname, SourceLoc(), "前驱缓存没有按块号严格升序排列");
          structurallySafe = false;
          break;
        }
      }
      const auto found = expectedPreds.find(block);
      const std::unordered_set<BasicBlock*> empty;
      const auto& expected = found == expectedPreds.end() ? empty : found->second;
      if (cached != expected) {
        add(cacheInvariant, fname, SourceLoc(), "前驱缓存与终结符重算的来源集合不一致");
        structurallySafe = false;
      }
    }

    // No indexed analysis or pointer arithmetic is allowed after a failed
    // structural preflight. The structural diagnostics above are complete.
    if (!structurallySafe) continue;

    if (const char* debug = std::getenv("SYSY_DBG_CFG"); debug != nullptr && *debug != '0') {
      for (BasicBlock* block : blocks) {
        Instruction* terminator = block->back();
        std::fprintf(stderr, "[CFG] %s L%u 终=%s 后继=%u", fname.c_str(),
                     static_cast<unsigned>(block->index()),
                     terminator != nullptr ? opcodeName(terminator->op()) : "(无)",
                     terminator != nullptr ? static_cast<unsigned>(terminator->numSuccs()) : 0u);
        if (terminator != nullptr) {
          for (size_t si = 0; si < terminator->numSuccs(); ++si) {
            std::fprintf(stderr, " L%u", static_cast<unsigned>(terminator->succ(si)->index()));
          }
        }
        std::fprintf(stderr, " | 前驱=%u", static_cast<unsigned>(block->numPreds()));
        for (BasicBlock* predecessor : block->preds()) {
          std::fprintf(stderr, " L%u", static_cast<unsigned>(predecessor->index()));
        }
        std::fprintf(stderr, "\n");
      }
    }

    // V4 function return signature is context dependent and therefore stays
    // outside the dependency leaf validator.
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* block = f->at(bi);
      Instruction* terminator = block->back();
      if (terminator == nullptr || terminator->op() != Opcode::Ret) continue;
      const bool voidFunction = f->retType() == nullptr || f->retType()->isVoid();
      if ((voidFunction && terminator->numOperands() != 0) ||
          (!voidFunction && terminator->numOperands() != 1) ||
          (!voidFunction && terminator->operand(0) != nullptr &&
           terminator->operand(0)->type() != f->retType())) {
        add("V4", fname, terminator->loc(), "ret 的值类型与函数返回类型不符");
      }
    }

    // V5: allocas remain in the entry block.
    BasicBlock* entry = f->entry();
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* block = f->at(bi);
      if (block == entry) continue;
      for (Instruction* instruction : block->insts()) {
        if (instruction->op() == Opcode::Alloca) {
          add("V5", fname, instruction->loc(),
              "alloca 出现在非入口块 L" + std::to_string(bi));
        }
      }
    }

    // V1: use-def consistency in both directions.
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* block = f->at(bi);
      for (Instruction* instruction : block->insts()) {
        std::unordered_set<const Instruction*> checkedDefinitions;
        for (Value* value : instruction->operands()) {
          if (value == nullptr) {
            add("V1", fname, instruction->loc(),
                std::string("操作数为空：") + opcodeName(instruction->op()));
            continue;
          }
          if (value->isInternedConstant()) continue;
          if (value->isInst()) {
            Instruction* definition = static_cast<Instruction*>(value);
            if (!checkedDefinitions.insert(definition).second) continue;
            if (definition->isConstant()) continue;
            if (definition->isParam()) {
              bool registered = false;
              for (Instruction* parameter : f->params()) {
                if (parameter == definition) { registered = true; break; }
              }
              if (!registered) {
                add("V1", fname, instruction->loc(), "操作数引用了未登记在当前函数中的形参");
                continue;
              }
              if (definition->paramOwner() != f) {
                add("V1", fname, instruction->loc(), "形参归属的函数与当前函数不一致");
                continue;
              }
            } else if (definition->parent() == nullptr || definition->parent()->parent() != f) {
              add("V1", fname, instruction->loc(), "操作数指向一条不属于当前函数的指令");
              continue;
            }
            const size_t actualUses = pointerCount(instruction->operands(), static_cast<Value*>(definition));
            const size_t reverseUses = pointerCount(definition->users(), instruction);
            if (actualUses != reverseUses) {
              add("V1", fname, instruction->loc(),
                  "use-def 使用次数不一致：操作数 " + std::to_string(actualUses) +
                      "，反向边 " + std::to_string(reverseUses));
            }
          } else if (value->isGlobalAddr()) {
            const auto* address = static_cast<const GlobalAddr*>(value);
            GlobalVariable* global = address->global();
            if (global == nullptr || moduleGlobals.count(global) == 0 ||
                global->objType() == nullptr || address->type() != global->addrType()) {
              add("V1", fname, instruction->loc(),
                  "全局地址为空、属于其他模块或地址类型与全局对象不一致");
            }
          } else if (!value->isBlockAddr()) {
            add("V1", fname, instruction->loc(), "操作数不是可用的值形态");
          }
        }
      }
    }
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      for (Instruction* definition : f->at(bi)->insts()) {
        std::unordered_set<const Instruction*> checkedUsers;
        for (Instruction* user : definition->users()) {
          if (!checkedUsers.insert(user).second) continue;
          const size_t reverseUses = pointerCount(definition->users(), user);
          const size_t actualUses = user == nullptr
                                        ? 0
                                        : pointerCount(user->operands(), static_cast<Value*>(definition));
          if (actualUses != reverseUses) {
            add("V1", fname, definition->loc(),
                "反向 use-def 使用次数不一致：操作数 " + std::to_string(actualUses) +
                    "，users() 边 " + std::to_string(reverseUses));
          }
        }
      }
    }
    for (size_t pi = 0; pi < f->numParams(); ++pi) {
      Instruction* parameter = f->param(pi);
      if (parameter == nullptr) {
        add("V1", fname, SourceLoc(), "形参表里有空项");
        continue;
      }
      if (!parameter->isParam()) add("V1", fname, parameter->loc(), "形参表中含有非形参指令");
      if (parameter->paramOwner() != f) {
        add("V1", fname, parameter->loc(), "形参归属的函数与登记它的函数不一致");
      }
      if (pointerCount(f->params(), parameter) != 1) {
        add("V1", fname, parameter->loc(), "形参表重复登记同一个形参");
      }
      for (Instruction* user : parameter->users()) {
        const size_t actualUses = user == nullptr
                                      ? 0
                                      : pointerCount(user->operands(), static_cast<Value*>(parameter));
        if (actualUses != pointerCount(parameter->users(), user)) {
          add("V1", fname, parameter->loc(), "形参的 use-def 使用次数不一致");
        }
      }
    }
    if (entry->numParams() != f->numParams()) {
      add("V1", fname, SourceLoc(), "入口块形参表与函数形参表长度不同");
    } else {
      for (size_t pi = 0; pi < f->numParams(); ++pi) {
        if (entry->param(pi) != f->param(pi)) {
          add("V1", fname, SourceLoc(), "入口块形参与函数形参表顺序不一致");
        }
      }
    }

    // V2: SSA definition dominance.
    DominatorTree dominators(*f);
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* block = f->at(bi);
      if (!dominators.reachable(bi)) continue;
      for (Instruction* instruction : block->insts()) {
        if (instruction->op() == Opcode::Phi) continue;
        for (Value* value : instruction->operands()) {
          if (value == nullptr || !value->isInst()) continue;
          Instruction* definition = static_cast<Instruction*>(value);
          const BasicBlock* definitionBlock = definition->isParam() ? f->entry() : definition->parent();
          if (definitionBlock == nullptr) continue;
          if (definitionBlock == block) {
            if (!definition->isParam() && !definedBefore(definition, instruction)) {
              add("V2", fname, instruction->loc(), "同一块内的定义没有先于使用");
            }
          } else if (!dominators.dominates(definitionBlock, block)) {
            add("V2", fname, instruction->loc(), "使用点不被定义支配");
          }
        }
      }
    }

    // V3: phi placement, count, exact predecessor coverage, and dominance.
    for (size_t bi = 0; bi < f->blockCount(); ++bi) {
      BasicBlock* block = f->at(bi);
      bool phiEnded = false;
      for (Instruction* instruction : block->insts()) {
        if (instruction->op() == Opcode::Phi) {
          if (phiEnded) add("V3", fname, instruction->loc(), "φ 不在块首连续排列");
        } else {
          phiEnded = true;
        }
      }
      for (Instruction* phi : block->insts()) {
        if (phi->op() != Opcode::Phi) continue;
        if (block == entry) {
          add("V3", fname, phi->loc(), "入口块不该有 φ");
          continue;
        }
        if (phi->numOperands() != block->numPreds()) {
          add("V3", fname, phi->loc(), "φ 的入值个数与前驱个数不符");
        }
        std::unordered_set<BasicBlock*> phiPreds;
        bool duplicatePhiPred = false;
        for (BasicBlock* predecessor : phi->succs()) {
          if (!phiPreds.insert(predecessor).second) duplicatePhiPred = true;
        }
        if (duplicatePhiPred) add("V3", fname, phi->loc(), "φ 的前驱含有重复来源");
        std::unordered_set<BasicBlock*> actualPreds(block->preds().begin(), block->preds().end());
        if (phiPreds != actualPreds) {
          add("V3", fname, phi->loc(), "φ 的入值块集合与块的前驱集合不同");
        }
        size_t selfReferences = 0;
        for (size_t pi = 0; pi < phi->numOperands(); ++pi) {
          Value* value = phi->operand(pi);
          BasicBlock* predecessor = phi->succ(pi);
          if (value == static_cast<Value*>(phi)) ++selfReferences;
          if (value == nullptr || predecessor == nullptr || !value->isInst()) continue;
          Instruction* definition = static_cast<Instruction*>(value);
          const BasicBlock* definitionBlock = definition->isParam() ? f->entry() : definition->parent();
          if (definitionBlock != nullptr && !dominators.dominates(definitionBlock, predecessor)) {
            add("V3", fname, phi->loc(), "φ 的入值不支配对应的前驱块");
          }
        }
        if (selfReferences == phi->numOperands() && phi->numOperands() != 0) {
          add("V3", fname, phi->loc(), "φ 的入值全部指向它自己");
        }
      }
    }
  }
  return bad;
}

std::string formatViolations(const std::vector<Violation>& violations) {
  std::string output;
  for (const Violation& violation : violations) {
    output += "  [" + violation.invariant + "] ";
    if (!violation.func.empty()) output += violation.func + ": ";
    if (violation.loc.line != 0) {
      output += "line " + std::to_string(violation.loc.line) + ": ";
    }
    output += violation.message + '\n';
  }
  return output;
}

}  // namespace flat
}  // namespace sysy
