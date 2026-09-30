// Private validation fragment included after the LLVM text helpers. Keeping
// this module-level input contract separate keeps the renderer below 600 lines.
bool safeSymbol(const std::string& name) {
  for (unsigned char c : name) if (c < 0x20 || c == 0x7f) return false;
  return true;
}
bool validType(const Type* ty, std::string& error, unsigned depth = 0) {
  if (ty == nullptr) { error = "null type in flat module"; return false; }
  if (depth > 64) { error = "type nesting exceeds emitter limit"; return false; }
  switch (ty->kind) {
    case TypeKind::Void: case TypeKind::I32: case TypeKind::I64: case TypeKind::F32: return true;
    case TypeKind::Ptr:
      if (ty->elem == nullptr || ty->elem->isVoid()) { error = "invalid pointer element type"; return false; }
      return validType(ty->elem, error, depth + 1);
    case TypeKind::Array:
      if (ty->len < 0 || ty->elem == nullptr || ty->elem->isVoid()) { error = "invalid array type"; return false; }
      return validType(ty->elem, error, depth + 1);
  }
  error = "unsupported type kind";
  return false;
}
bool sameType(const Type* a, const Type* b) {
  const bool av = a == nullptr || a->isVoid();
  const bool bv = b == nullptr || b->isVoid();
  return av || bv ? av == bv : llvmType(a) == llvmType(b);
}
bool preflight(const Module& module, std::string& error) {
  std::unordered_set<std::string> symbols;
  std::unordered_set<const GlobalVariable*> globalObjects;
  std::unordered_map<std::string, const Function*> functions;
  for (size_t i = 0; i < module.numGlobals(); ++i) {
    const GlobalVariable* g = module.global(i);
    if (g == nullptr || !safeSymbol(g->name()) || g->name().empty() || !symbols.insert(g->name()).second ||
        g->objType() == nullptr || !validType(g->objType(), error) || g->objType()->isVoid() || g->objType()->isPtr()) {
      if (error.empty()) error = "invalid, null, or duplicate global declaration";
      return false;
    }
    globalObjects.insert(g);
    const int64_t bytes = typeByteSize(g->objType());
    if (bytes < 0) { error = "global object size overflows the supported address range"; return false; }
    for (const auto& item : g->initData()) if ((item.first % 4) != 0 || item.first > static_cast<uint64_t>(bytes) ||
          (bytes < 4 && !g->initData().empty()) || item.first + 4 > static_cast<uint64_t>(bytes)) {
        error = "global initializer has an invalid sparse byte offset"; return false;
    }
  }
  for (size_t i = 0; i < module.numFunctions(); ++i) {
    const Function* f = module.function(i);
    if (f == nullptr || !safeSymbol(f->name()) || !validSymbol(f->name()) || !symbols.insert(f->name()).second ||
        !functions.emplace(f->name(), f).second || f->retType() == nullptr || !validType(f->retType(), error)) {
      if (error.empty()) error = "invalid, null, or duplicate function declaration";
      return false;
    }
    if (!f->isDeclaration() && f->name() == "putf") {
      error = "putf has a fixed variadic runtime ABI and cannot be defined in flat IR";
      return false;
    }
    if (f->isDeclaration() && f->name() == "putf" &&
        (!f->retType()->isVoid() || f->numParams() != 0)) {
      error = "putf declaration must use the runtime placeholder signature (void, no flat params)";
      return false;
    }
    if (!f->retType()->isVoid() && !f->retType()->isInt() && !f->retType()->isFloat()) {
      error = "function return type is outside the supported ABI subset"; return false;
    }
    for (Instruction* p : f->params()) if (p == nullptr || p->type() == nullptr || !validType(p->type(), error) ||
          (!p->type()->isInt() && !p->type()->isFloat() && !p->type()->isPtr())) {
        error = "function parameter type is outside the supported ABI subset"; return false;
    }
    if (f->isDeclaration()) continue;
    for (size_t bi = 0; bi < f->blocks().size(); ++bi) {
      const BasicBlock* b = f->blocks()[bi];
      if (b == nullptr || b->parent() != f || b->index() != bi) {
        error = "function contains a null or mis-indexed basic block"; return false;
      }
      for (Instruction* inst : b->insts()) {
        if (inst == nullptr || inst->parent() != b || inst->isConstant()) {
          error = "function contains a null, constant, or unsupported instruction"; return false;
        }
        if (inst->type() != nullptr && !validType(inst->type(), error)) return false;
        for (Value* v : inst->operands()) {
          if (v == nullptr || v->type() == nullptr || !validType(v->type(), error) || v->isBlockAddr()) {
            error = "instruction contains a null or unsupported operand"; return false;
          }
          if (v->isGlobalAddr()) {
            const auto* address = static_cast<const GlobalAddr*>(v);
            const GlobalVariable* global = address->global();
            if (global == nullptr || globalObjects.count(global) == 0 ||
                address->type() != global->addrType()) {
              error = "global address is null, foreign, or has a mismatched type";
              return false;
            }
          }
          if (v->isInst()) {
            Instruction* iv = static_cast<Instruction*>(v);
            bool localParam = false;
            for (Instruction* p : f->params()) if (p == iv) localParam = true;
            if ((!iv->isConstant() && iv->parent() == nullptr && !localParam) ||
                (iv->parent() != nullptr && iv->parent()->parent() != f)) {
              error = "instruction operand belongs to another function"; return false;
            }
          }
        }
        for (BasicBlock* s : inst->succs()) if (s == nullptr || s->parent() != f) {
          error = "instruction has a null or foreign successor"; return false;
        }
        if (!validateFlatInstruction(*inst, error)) return false;
        if (inst->op() == Opcode::Ret) {
          const bool voidRet = f->retType()->isVoid();
          if ((voidRet && inst->numOperands() != 0) || (!voidRet && inst->numOperands() != 1) ||
              (!voidRet && !sameType(inst->operand(0)->type(), f->retType()))) {
            error = "ret result type does not match function signature"; return false;
          }
        }
        if ((inst->op() == Opcode::LLVMMemCpy && inst->callee() != "llvm.memcpy") ||
            (inst->op() == Opcode::LLVMMemSet && inst->callee() != "llvm.memset")) {
          error = "intrinsic opcode has a mismatched callee"; return false;
        }
        if (inst->op() == Opcode::Call && (inst->callee() == "llvm.memcpy" || inst->callee() == "llvm.memset")) {
          // FlatReader represents intrinsic calls as Opcode::Call after parsing.
          continue;
        }
        if (inst->op() == Opcode::Call && inst->callee() == "putf") {
          if (inst->type() != nullptr || inst->numOperands() == 0 ||
              inst->operand(0)->type() == nullptr || !inst->operand(0)->type()->isPtr()) {
            error = "putf requires a void call with a leading format pointer"; return false;
          }
          continue;
        }
        const auto target = functions.find(inst->callee());
        if (inst->op() == Opcode::Call && target != functions.end()) {
          const Function* callee = target->second;
          if (!sameType(inst->type(), callee->retType()) || inst->numOperands() != callee->numParams()) {
            error = "call signature does not match function @" + inst->callee(); return false;
          }
          for (size_t ai = 0; ai < inst->numOperands(); ++ai) if (!sameType(inst->operand(ai)->type(), callee->paramType(ai))) {
              error = "call argument type does not match function @" + inst->callee(); return false;
          }
        }
      }
    }
  }
  // FPExt carries an LLVM double value while flat IR retains the source f32
  // type. It is only representable at an external call ABI boundary; using it
  // in another flat instruction would silently re-type the value as float.
  for (size_t fi = 0; fi < module.numFunctions(); ++fi) {
    const Function* f = module.function(fi);
    if (f == nullptr || f->isDeclaration()) continue;
    for (const BasicBlock* block : f->blocks()) {
      for (const Instruction* inst : block->insts()) {
        if (inst->op() != Opcode::FPExt) continue;
        bool usedAtAbiBoundary = false;
        for (size_t uf = 0; uf < module.numFunctions(); ++uf) {
          const Function* userFunction = module.function(uf);
          if (userFunction == nullptr || userFunction->isDeclaration()) continue;
          for (const BasicBlock* userBlock : userFunction->blocks()) {
            for (const Instruction* user : userBlock->insts()) {
              for (Value* operand : user->operands()) {
                if (operand != inst) continue;
                size_t argument = user->numOperands();
                for (size_t ai = 0; ai < user->numOperands(); ++ai) {
                  if (user->operand(ai) == inst) { argument = ai; break; }
                }
                if (user->op() != Opcode::Call || user->callee() != "putf" || argument == 0) {
                  error = "fpext double result is only supported in a putf variadic argument";
                  return false;
                }
                usedAtAbiBoundary = true;
              }
            }
          }
        }
        if (!usedAtAbiBoundary) {
          error = "fpext double result must be used at an external call ABI boundary";
          return false;
        }
      }
    }
  }
  return true;
}
