#include "ir/LLVMTextEmitter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/Module.h"
#include "ir/FlatSemantics.h"

namespace sysy {
namespace flat {
namespace {
constexpr uint64_t kMaxExpandedGlobalElements = 1000000;
std::string llvmType(const Type* ty) {
  if (ty == nullptr) return "<null-type>";
  switch (ty->kind) {
    case TypeKind::Void: return "void";
    case TypeKind::I32: return "i32";
    case TypeKind::I64: return "i64";
    case TypeKind::F32: return "float";
    case TypeKind::Ptr: return "ptr";
    case TypeKind::Array:
      return "[" + std::to_string(ty->len) + " x " + llvmType(ty->elem) + "]";
  }
  return "<bad-type>";
}
std::string llvmReturnType(const Type* ty) {
  return ty == nullptr ? "void" : llvmType(ty);
}
std::string llvmValueType(const Value* value) {
  if (value != nullptr && value->isInst() &&
      static_cast<const Instruction*>(value)->op() == Opcode::FPExt) return "double";
  return value == nullptr ? "<null-type>" : llvmType(value->type());
}
bool validSymbol(const std::string& name) {
  if (name.empty()) return false;
  const auto first = static_cast<unsigned char>(name[0]);
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
        first == '_' || first == '$' || first == '.')) return false;
  for (unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.' || c == '-')) {
      return false;
    }
  }
  return true;
}
std::string symbolRef(const std::string& name) {
  if (validSymbol(name)) return "@" + name;
  std::string result = "@\"";
  static constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : name) {
    if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) {
      result += '\\'; result += hex[c >> 4]; result += hex[c & 0x0f];
    } else {
      result += static_cast<char>(c);
    }
  }
  result += '"';
  return result;
}
std::string floatLiteral(uint32_t bits) {
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  double d = static_cast<double>(f);
  uint64_t dbits = 0;
  std::memcpy(&dbits, &d, sizeof(dbits));
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << dbits;
  return out.str();
}
std::string integerLiteral(const Type* ty, int64_t value) {
  if (ty != nullptr && ty->kind == TypeKind::I32) {
    return std::to_string(static_cast<int32_t>(value));
  }
  return std::to_string(value);
}
struct FunctionNames {
  std::unordered_map<const Value*, std::string> names;
  uint32_t nextValue = 0;
  mutable uint32_t nextTemp = 0;

  void number(const Function& function) {
    for (Argument* param : function.params()) {
      names[param] = "%v" + std::to_string(nextValue++);
    }
    for (BasicBlock* block : function.blocks()) {
      for (Instruction* inst : block->insts()) {
        if (inst->hasResult()) {
          names[inst] = "%v" + std::to_string(nextValue++);
        }
      }
    }
  }

  std::string value(const Value* value) const {
    if (value == nullptr) return "<null-value>";
    if (value->isGlobalAddr()) {
      const auto* address = static_cast<const GlobalAddr*>(value);
      return address->global() == nullptr ? "<null-global>" : symbolRef(address->global()->name());
    }
    if (value->isBlockAddr()) return "<block-address>";
    if (value->isConstant()) {
      const auto* constant = static_cast<const Constant*>(value);
      return value->type() != nullptr && value->type()->kind == TypeKind::F32
          ? floatLiteral(constant->fbits())
          : integerLiteral(value->type(), constant->intVal());
    }
    const auto it = names.find(value);
    return it == names.end() ? "<unnamed-value>" : it->second;
  }

  std::string temp() const { return "%t" + std::to_string(nextTemp++); }
};
bool hasResult(const Instruction& inst) {
  return inst.hasResult();
}
void appendTypedValue(std::string& out, const Value* value, const FunctionNames& names) {
  out += llvmType(value == nullptr ? nullptr : value->type());
  out += ' ';
  out += names.value(value);
}
bool expectOperands(const Instruction& inst, size_t count, std::string& error) {
  if (inst.numOperands() == count) return true;
  error = "malformed " + std::string(opcodeName(inst.op())) + " instruction: expected " +
          std::to_string(count) + " operands";
  return false;
}
bool integerConstant(const Value* value, int64_t& result) {
  if (value == nullptr) return false;
  if (value->isConstant()) {
    const auto* constant = static_cast<const Constant*>(value);
    if (constant->type() == nullptr || constant->type()->kind == TypeKind::F32) return false;
    result = constant->intVal();
    return true;
  }
  return false;
}
bool conditionValue(std::string& out, const Value* condition,
                    const FunctionNames& names, std::string& result) {
  if (condition == nullptr || condition->type() == nullptr || !condition->type()->isInt()) {
    return false;
  }
  result = names.temp();
  out += "  " + result + " = icmp ne i32 " + names.value(condition) + ", 0\n";
  return true;
}
bool appendInstruction(std::string& out, const Instruction& inst,
                       const FunctionNames& names, std::string& error) {
  const Opcode op = inst.op();
  for (Value* operand : inst.operands()) {
    if (operand == nullptr || operand->type() == nullptr || operand->isBlockAddr() ||
        (operand->isGlobalAddr() && static_cast<const GlobalAddr*>(operand)->global() == nullptr)) {
      error = "instruction has a null or unsupported operand";
      return false;
    }
  }
  const std::string result = hasResult(inst) ? names.value(&inst) + " = " : "";
  std::string line = "  " + result;
  const size_t n = inst.numOperands();
  auto typed = [&](size_t index) { appendTypedValue(line, inst.operand(index), names); };
  switch (op) {
    case Opcode::Ret:
      if (n == 0) line += "ret void";
      else if (n == 1) { line += "ret "; typed(0); }
      else return expectOperands(inst, 1, error);
      break;
    case Opcode::Br:
      if (inst.numSuccs() == 1) {
        if (n != 0) return expectOperands(inst, 0, error);
        line += "br label %bb" + std::to_string(inst.succ(0)->index());
      } else if (inst.numSuccs() == 2 && n == 1) {
        std::string cond;
        if (!conditionValue(out, inst.operand(0), names, cond)) {
          error = "conditional branch requires an i32 condition";
          return false;
        }
        line += "br i1 " + cond + ", label %bb" + std::to_string(inst.succ(0)->index()) +
                ", label %bb" + std::to_string(inst.succ(1)->index());
      } else {
        error = "malformed branch instruction";
        return false;
      }
      break;
    case Opcode::Unreachable:
      if (!expectOperands(inst, 0, error)) return false;
      line += "unreachable";
      break;
    case Opcode::Alloca:
      if (n != 0 || inst.srcElemType() == nullptr || inst.srcElemType()->isVoid()) {
        error = "malformed alloca instruction";
        return false;
      }
      line += "alloca " + llvmType(inst.srcElemType()) + ", align 4";
      break;
    case Opcode::Load:
      if (!expectOperands(inst, 1, error)) return false;
      line += "load " + llvmType(inst.type()) + ", ptr " + names.value(inst.operand(0)) + ", align 4";
      break;
    case Opcode::Store:
      if (!expectOperands(inst, 2, error)) return false;
      line += "store "; typed(0); line += ", ptr "; line += names.value(inst.operand(1));
      line += ", align 4";
      break;
    case Opcode::GEP:
      if (!expectOperands(inst, 2, error)) return false;
      if (inst.srcElemType() == nullptr || inst.srcElemType()->isVoid()) {
        error = "getelementptr has no source element type";
        return false;
      }
      line += "getelementptr " + llvmType(inst.srcElemType()) + ", ptr " +
              names.value(inst.operand(0)) + ", i64 " + names.value(inst.operand(1));
      break;
    case Opcode::ICmp:
    case Opcode::FCmp: {
      if (!expectOperands(inst, 2, error)) return false;
      const std::string raw = names.temp();
      const std::string opName = op == Opcode::ICmp ? "icmp" : "fcmp";
      const std::string pred = op == Opcode::ICmp ? ipredName(inst.ipred()) : fpredName(inst.fpred());
      out += "  " + raw + " = " + opName + " " + pred + " " +
             llvmType(inst.operand(0)->type()) + " " + names.value(inst.operand(0)) + ", " +
             names.value(inst.operand(1)) + "\n";
      line += "zext i1 " + raw + " to i32";
      break;
    }
    case Opcode::SIToFP: case Opcode::FPToSI: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc:
      if (!expectOperands(inst, 1, error)) return false;
      line += opcodeName(op); line += ' ';
      if (op == Opcode::FPExt) {
        line += llvmType(inst.operand(0)->type()); line += ' ';
        line += names.value(inst.operand(0));
      } else {
        typed(0);
      }
      line += " to ";
      line += op == Opcode::FPExt ? "double" : llvmType(inst.type());
      break;
    case Opcode::BitCast:
      if (!expectOperands(inst, 1, error)) return false;
      if (inst.operand(0)->type() == nullptr || inst.type() == nullptr ||
          !inst.operand(0)->type()->isPtr() || !inst.type()->isPtr()) {
        error = "bitcast subset only supports pointer-to-pointer casts";
        return false;
      }
      line += "getelementptr i8, ptr " + names.value(inst.operand(0)) + ", i64 0";
      break;
    case Opcode::Call: case Opcode::LLVMMemCpy: case Opcode::LLVMMemSet: {
      if (!validSymbol(inst.callee())) { error = "invalid callee name in call"; return false; }
      if ((inst.callee() == "llvm.memcpy" || inst.callee() == "llvm.memset") &&
          inst.type() != nullptr) {
        error = "LLVM memory intrinsics must return void";
        return false;
      }
      if (inst.callee() == "llvm.memcpy") {
        if (n != 4 || inst.operand(0)->type() == nullptr || !inst.operand(0)->type()->isPtr() ||
            inst.operand(1)->type() == nullptr || !inst.operand(1)->type()->isPtr() ||
            inst.operand(2)->type() == nullptr || inst.operand(2)->type()->kind != TypeKind::I64 ||
            inst.operand(3)->type() == nullptr || !inst.operand(3)->type()->isInt()) {
          error = "llvm.memcpy requires (ptr, ptr, i64, i32 alignment)";
          return false;
        }
        int64_t alignment = 0;
        if (!integerConstant(inst.operand(3), alignment) || alignment != 1) {
          error = "llvm.memcpy alignment operand must be the supported value 1";
          return false;
        }
        line += "call void @llvm.memcpy.p0.p0.i64(ptr ";
        line += names.value(inst.operand(0)); line += ", ptr ";
        line += names.value(inst.operand(1)); line += ", i64 ";
        line += names.value(inst.operand(2)); line += ", i1 false)";
        break;
      }
      if (inst.callee() == "llvm.memset") {
        if (n != 4 || inst.operand(0)->type() == nullptr || !inst.operand(0)->type()->isPtr() ||
            inst.operand(1)->type() == nullptr || !inst.operand(1)->type()->isInt() ||
            inst.operand(2)->type() == nullptr || inst.operand(2)->type()->kind != TypeKind::I64 ||
            inst.operand(3)->type() == nullptr || !inst.operand(3)->type()->isInt()) {
          error = "llvm.memset requires (ptr, i32, i64, i32 alignment)";
          return false;
        }
        int64_t alignment = 0;
        if (!integerConstant(inst.operand(3), alignment) || alignment != 1) {
          error = "llvm.memset alignment operand must be the supported value 1";
          return false;
        }
        std::string byteValue = names.value(inst.operand(1));
        if (inst.operand(1)->isInst()) {
          byteValue = names.temp();
          out += "  " + byteValue + " = trunc i32 " + names.value(inst.operand(1)) + " to i8\n";
        } else {
          int64_t integer = 0;
          if (!integerConstant(inst.operand(1), integer)) {
            error = "llvm.memset byte value is not an integer constant or i32 value";
            return false;
          }
          byteValue = std::to_string(static_cast<int32_t>(integer) & 255);
        }
        line += "call void @llvm.memset.p0.i64(ptr ";
        line += names.value(inst.operand(0)); line += ", i8 "; line += byteValue;
        line += ", i64 "; line += names.value(inst.operand(2)); line += ", i1 false)";
        break;
      }
      line += "call ";
      line += llvmReturnType(inst.type()); line += ' '; line += symbolRef(inst.callee()); line += '(';
      for (size_t i = 0; i < n; ++i) {
        if (i != 0) line += ", ";
        line += llvmValueType(inst.operand(i)); line += ' ';
        line += names.value(inst.operand(i));
      }
      line += ')';
      break;
    }
    case Opcode::Phi:
      if (n != inst.numSuccs() || n == 0) { error = "malformed phi instruction"; return false; }
      line += "phi "; line += llvmType(inst.type()); line += ' ';
      for (size_t i = 0; i < n; ++i) {
        if (i != 0) line += ", ";
        line += "[ "; line += names.value(inst.operand(i)); line += ", %bb";
        line += std::to_string(inst.succ(i)->index()); line += " ]";
      }
      break;
    case Opcode::Select: {
      if (!expectOperands(inst, 3, error)) return false;
      std::string cond;
      if (!conditionValue(out, inst.operand(0), names, cond)) {
        error = "select requires an i32 condition";
        return false;
      }
      line += "select i1 "; line += cond; line += ", "; typed(1); line += ", "; typed(2);
      break;
    }
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::SDiv:
    case Opcode::SRem: case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
      if (!expectOperands(inst, 2, error)) return false;
      line += opcodeName(op); line += ' '; line += llvmType(inst.type()); line += ' ';
      line += names.value(inst.operand(0)); line += ", "; line += names.value(inst.operand(1));
      break;
    case Opcode::FNeg:
      if (!expectOperands(inst, 1, error)) return false;
      line += "fneg float "; line += names.value(inst.operand(0));
      break;
    default:
      error = "unsupported flat opcode: " + std::string(opcodeName(op));
      return false;
  }
  out += line;
  out += '\n';
  return true;
}

using InitialData = std::map<uint64_t, uint32_t>;
bool emitInitializer(std::string& out, const Type* ty, uint64_t offset,
                     const InitialData& data, uint64_t& emitted) {
  const int64_t size = typeByteSize(ty);
  if (size < 0) return false;
  auto first = data.lower_bound(offset);
  if (first == data.end() || first->first >= offset + static_cast<uint64_t>(size)) {
    out += "zeroinitializer";
    return true;
  }
  if (ty->kind == TypeKind::I32 || ty->kind == TypeKind::F32) {
    auto it = data.find(offset);
    if (it == data.end()) { out += ty->kind == TypeKind::F32 ? "0.000000e+00" : "0"; }
    else if (ty->kind == TypeKind::F32) { out += floatLiteral(it->second); }
    else { out += std::to_string(static_cast<int32_t>(it->second)); }
    return true;
  }
  if (ty->kind != TypeKind::Array || ty->elem == nullptr || ty->len < 0) return false;
  const int64_t elemSize = typeByteSize(ty->elem);
  if (elemSize <= 0 || static_cast<uint64_t>(ty->len) > kMaxExpandedGlobalElements - emitted) {
    return false;
  }
  emitted += static_cast<uint64_t>(ty->len);
  out += "[";
  for (int64_t i = 0; i < ty->len; ++i) {
    if (i != 0) out += ", ";
    out += llvmType(ty->elem); out += ' ';
    if (!emitInitializer(out, ty->elem, offset + static_cast<uint64_t>(i) *
                         static_cast<uint64_t>(elemSize), data, emitted)) return false;
  }
  out += "]";
  return true;
}
bool appendGlobal(std::string& out, const GlobalVariable& global, std::string& error) {
  if (global.name().empty() || global.objType() == nullptr ||
      global.objType()->isVoid() || global.objType()->isPtr()) {
    error = "unsupported global object or name";
    return false;
  }
  out += symbolRef(global.name()) + " = global " + llvmType(global.objType()) + ' ';
  if (!global.hasData()) {
    out += "zeroinitializer";
  } else {
    InitialData data;
    for (const auto& item : global.initData()) {
      if ((item.first % 4) != 0 || item.first > static_cast<uint64_t>(typeByteSize(global.objType())) - 4 ||
          !data.emplace(item.first, item.second).second) {
        error = "global initializer has duplicate or out-of-range data";
        return false;
      }
    }
    uint64_t emitted = 0;
    if (!emitInitializer(out, global.objType(), 0, data, emitted)) {
      error = "global initializer exceeds the supported aggregate emission budget";
      return false;
    }
  }
  out += ", align 4\n";
  return true;
}
#include "ir/LLVMTextEmitterValidation.inc"
struct Declaration {
  const Type* result = nullptr;
  std::vector<std::string> params;
  bool variadic = false;
};

bool sameDeclaration(const Declaration& left, const Declaration& right) {
  if (llvmReturnType(left.result) != llvmReturnType(right.result) ||
      left.params.size() != right.params.size() || left.variadic != right.variadic) return false;
  for (size_t i = 0; i < left.params.size(); ++i) {
    if (left.params[i] != right.params[i]) return false;
  }
  return true;
}

bool collectDeclarations(const Module& module, std::unordered_map<std::string, Declaration>& decls,
                         bool& hasMemcpy, bool& hasMemset, std::string& error) {
  std::unordered_set<std::string> defined;
  for (Function* fn : module.functions()) {
    if (fn != nullptr && !fn->isDeclaration()) defined.insert(fn->name());
  }
  for (Function* fn : module.functions()) {
    if (fn == nullptr || fn->isDeclaration()) continue;
    for (BasicBlock* block : fn->blocks()) {
      for (Instruction* inst : block->insts()) {
        if (inst->op() != Opcode::Call && inst->op() != Opcode::LLVMMemCpy &&
            inst->op() != Opcode::LLVMMemSet) continue;
        if (inst->op() == Opcode::LLVMMemCpy || inst->callee() == "llvm.memcpy") {
          hasMemcpy = true;
          continue;
        }
        if (inst->op() == Opcode::LLVMMemSet || inst->callee() == "llvm.memset") {
          hasMemset = true;
          continue;
        }
        if (inst->op() != Opcode::Call) continue;
        const std::string& name = inst->callee();
        if (!validSymbol(name)) { error = "invalid callee name in call"; return false; }
        if (defined.count(name) != 0) continue;
        Declaration candidate;
        candidate.result = inst->type();
        if (name == "putf") {
          if (inst->type() != nullptr || inst->numOperands() == 0 ||
              inst->operand(0)->type() == nullptr || !inst->operand(0)->type()->isPtr()) {
            error = "putf requires a void call with a leading format pointer";
            return false;
          }
          candidate.params.push_back("ptr");
          candidate.variadic = true;
          for (size_t ai = 1; ai < inst->numOperands(); ++ai) {
            const Type* type = inst->operand(ai)->type();
            if (type == nullptr || (!type->isInt() && !type->isFloat()) ||
                (type->isFloat() && llvmValueType(inst->operand(ai)) != "double")) {
              error = "putf variadic arguments must be integers or promoted floats";
              return false;
            }
          }
        } else {
          for (Value* value : inst->operands()) candidate.params.push_back(llvmValueType(value));
        }
        auto existing = decls.find(name);
        if (existing == decls.end()) decls.emplace(name, candidate);
        else {
          const Declaration& prior = existing->second;
          if (!sameDeclaration(prior, candidate)) {
            error = "conflicting call signatures for external function @" + name;
            return false;
          }
        }
      }
    }
  }
  for (Function* fn : module.functions()) {
    if (fn == nullptr || !fn->isDeclaration()) continue;
    if (fn->name() == "putf") continue;  // The closed ABI contract is variadic.
    if (!validSymbol(fn->name())) { error = "invalid function declaration name"; return false; }
    Declaration candidate;
    candidate.result = fn->retType();
    for (Argument* param : fn->params()) candidate.params.push_back(llvmType(param->type()));
    auto existing = decls.find(fn->name());
    if (existing == decls.end()) decls.emplace(fn->name(), candidate);
    else if (!sameDeclaration(existing->second, candidate)) {
      error = "conflicting declaration for @" + fn->name();
      return false;
    }
  }
  return true;
}

bool appendFunction(std::string& out, const Function& function, std::string& error) {
  if (!validSymbol(function.name()) || function.retType() == nullptr) {
    error = "unsupported function name or return type";
    return false;
  }
  FunctionNames names;
  names.number(function);
  out += "define " + llvmReturnType(function.retType()) + ' ' + symbolRef(function.name()) + '(';
  for (size_t i = 0; i < function.numParams(); ++i) {
    if (i != 0) out += ", ";
    out += llvmType(function.paramType(i)); out += ' ';
    out += names.value(function.param(i));
  }
  out += ") {\n";
  for (BasicBlock* block : function.blocks()) {
    out += "bb" + std::to_string(block->index()) + ":\n";
    for (Instruction* inst : block->insts()) {
      if (!appendInstruction(out, *inst, names, error)) return false;
    }
  }
  out += "}\n";
  return true;
}

}  // namespace

bool LLVMTextEmitter::emit(const Module& module, std::string& output,
                           std::string& error) const {
  std::string candidate;
  std::unordered_map<std::string, Declaration> declarations;
  bool hasMemcpy = false;
  bool hasMemset = false;
  error.clear();
  if (!preflight(module, error)) return false;
  for (size_t i = 0; i < module.numGlobals(); ++i) {
    GlobalVariable* global = module.global(i);
    if (!appendGlobal(candidate, *global, error)) return false;
  }
  if (!collectDeclarations(module, declarations, hasMemcpy, hasMemset, error)) return false;
  if (hasMemcpy) {
    candidate += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
  }
  if (hasMemset) {
    candidate += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n";
  }
  std::vector<std::string> names;
  names.reserve(declarations.size());
  for (const auto& item : declarations) names.push_back(item.first);
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const Declaration& decl = declarations.at(name);
    candidate += "declare " + llvmReturnType(decl.result) + ' ' + symbolRef(name) + '(';
    for (size_t i = 0; i < decl.params.size(); ++i) {
      if (i != 0) candidate += ", ";
      candidate += decl.params[i];
    }
    if (decl.variadic) {
      if (!decl.params.empty()) candidate += ", ";
      candidate += "...";
    }
    candidate += ")\n";
  }
  for (size_t i = 0; i < module.numFunctions(); ++i) {
    Function* function = module.function(i);
    if (function != nullptr && !function->isDeclaration() &&
        !appendFunction(candidate, *function, error)) return false;
  }
  output.swap(candidate);
  return true;
}

}  // namespace flat
}  // namespace sysy
