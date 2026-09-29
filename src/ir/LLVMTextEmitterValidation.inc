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
bool supportedOpcode(Opcode op) {
  switch (op) {
    case Opcode::Ret: case Opcode::Br: case Opcode::Unreachable: case Opcode::Alloca: case Opcode::Load:
    case Opcode::Store: case Opcode::GEP: case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::SDiv:
    case Opcode::SRem: case Opcode::Shl: case Opcode::LShr: case Opcode::AShr: case Opcode::And: case Opcode::Or:
    case Opcode::Xor: case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv: case Opcode::FNeg:
    case Opcode::ICmp: case Opcode::FCmp: case Opcode::SIToFP: case Opcode::FPToSI: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc: case Opcode::BitCast: case Opcode::Call:
    case Opcode::LLVMMemCpy: case Opcode::LLVMMemSet: case Opcode::Phi: case Opcode::Select:
      return true;
    default: return false;
  }
}
bool instructionContract(const Instruction& i, std::string& error) {
  const Type* i32 = typePool().i32(); const Type* i64 = typePool().i64();
  const Type* f32 = typePool().f32(); const Type* vty = i.type();
  auto fail = [&](const char* text) { error = std::string(opcodeName(i.op())) + ": " + text; return false; };
  auto bin = [&](const Type* ty) {
    return i.numOperands() == 2 && sameType(vty, ty) && i.operand(0)->type() == ty && i.operand(1)->type() == ty;
  };
  const Opcode op = i.op();
  if (op == Opcode::Ret) return i.numOperands() <= 1 ? true : fail("return arity is invalid");
  if (op == Opcode::Br) return (i.numSuccs() == 1 && i.numOperands() == 0) ||
      (i.numSuccs() == 2 && i.numOperands() == 1 && i.operand(0)->type() == i32) ? true : fail("branch shape or condition type is invalid");
  if (op == Opcode::Unreachable) return i.numOperands() == 0 ? true : fail("takes no operands");
  if (op == Opcode::Alloca) return i.numOperands() == 0 && i.srcElemType() != nullptr && !i.srcElemType()->isVoid() &&
      vty != nullptr && vty->isPtr() && sameType(vty->elem, i.srcElemType()) ? true : fail("alloca result/object type mismatch");
  if (op == Opcode::Load) return i.numOperands() == 1 && vty != nullptr && !vty->isVoid() && i.operand(0)->type()->isPtr() &&
      sameType(i.operand(0)->type()->elem, vty) ? true : fail("load type does not match pointer element type");
  // Store.srcElemType preserves address-calculation metadata in flat IR; it can
  // describe an enclosing array. The stored value and typed pointer element are
  // the actual LLVM store contract.
  if (op == Opcode::Store) return i.numOperands() == 2 && i.operand(1)->type()->isPtr() &&
      sameType(i.operand(0)->type(), i.operand(1)->type()->elem) ? true : fail("store value/pointer element mismatch");
  if (op == Opcode::GEP) return i.numOperands() == 2 && i.srcElemType() != nullptr && i.operand(0)->type()->isPtr() &&
      i.operand(1)->type() == i64 && vty != nullptr && vty->isPtr() && sameType(vty->elem, i.srcElemType()) ? true : fail("gep type mismatch");
  if (op >= Opcode::Add && op <= Opcode::Xor) return bin(i32) ? true : fail("integer operands/result must be i32");
  if (op >= Opcode::FAdd && op <= Opcode::FDiv) return bin(f32) ? true : fail("float operands/result must be float");
  if (op == Opcode::FNeg) return i.numOperands() == 1 && vty == f32 && i.operand(0)->type() == f32 ? true : fail("fneg type is invalid");
  if (op == Opcode::ICmp) return i.numOperands() == 2 && vty == i32 && i.operand(0)->type() == i32 && i.operand(1)->type() == i32 ? true : fail("icmp type is invalid");
  if (op == Opcode::FCmp) return i.numOperands() == 2 && vty == i32 && i.operand(0)->type() == f32 && i.operand(1)->type() == f32 ? true : fail("fcmp type is invalid");
  if (op >= Opcode::SIToFP && op <= Opcode::Trunc) {
    if (i.numOperands() != 1) return fail("cast arity is invalid");
    const Type* src = i.operand(0)->type();
    if (op == Opcode::SIToFP) return src == i32 && vty == f32 ? true : fail("sitofp types are invalid");
    if (op == Opcode::FPToSI) return src == f32 && vty == i32 ? true : fail("fptosi types are invalid");
    if (op == Opcode::FPExt) return src == f32 && vty == f32 ? true : fail("fpext representation is invalid");
    if (op == Opcode::Trunc) return src == i64 && vty == i32 ? true : fail("trunc types are invalid");
    return src == i32 && vty == i64 ? true : fail("integer extension types are invalid");
  }
  if (op == Opcode::BitCast) return i.numOperands() == 1 && i.operand(0)->type()->isPtr() && vty != nullptr && vty->isPtr() ? true : fail("bitcast supports pointers only");
  if (op == Opcode::Phi) {
    if (i.numOperands() == 0 || i.numOperands() != i.numSuccs() || vty == nullptr) return fail("phi shape is invalid");
    for (Value* value : i.operands()) if (!sameType(value->type(), vty)) return fail("phi incoming type mismatch");
    return true;
  }
  if (op == Opcode::Select) return i.numOperands() == 3 && i.operand(0)->type() == i32 && sameType(i.operand(1)->type(), vty) && sameType(i.operand(2)->type(), vty) ? true : fail("select type mismatch");
  return op == Opcode::Call || op == Opcode::LLVMMemCpy || op == Opcode::LLVMMemSet ? true : fail("unsupported opcode");
}
bool preflight(const Module& module, std::string& error) {
  std::unordered_set<std::string> symbols;
  std::unordered_map<std::string, const Function*> functions;
  for (size_t i = 0; i < module.numGlobals(); ++i) {
    const GlobalVariable* g = module.global(i);
    if (g == nullptr || !safeSymbol(g->name()) || g->name().empty() || !symbols.insert(g->name()).second ||
        g->objType() == nullptr || !validType(g->objType(), error) || g->objType()->isVoid() || g->objType()->isPtr()) {
      if (error.empty()) error = "invalid, null, or duplicate global declaration";
      return false;
    }
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
        if (inst == nullptr || inst->parent() != b || !supportedOpcode(inst->op()) || inst->isConstant()) {
          error = "function contains a null, constant, or unsupported instruction"; return false;
        }
        if (inst->type() != nullptr && !validType(inst->type(), error)) return false;
        for (Value* v : inst->operands()) {
          if (v == nullptr || v->type() == nullptr || !validType(v->type(), error) || v->isBlockAddr() ||
              (v->isGlobalAddr() && static_cast<GlobalAddr*>(v)->global() == nullptr)) {
            error = "instruction contains a null or unsupported operand"; return false;
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
        if (!instructionContract(*inst, error)) return false;
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
