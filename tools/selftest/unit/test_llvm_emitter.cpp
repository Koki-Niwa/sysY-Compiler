#include <cstdio>
#include <string>

#include "ir/LLVMTextEmitter.h"
#include "ir/Module.h"

namespace {

int failures = 0;

void expect(bool condition, const char* label) {
  std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
  if (!condition) ++failures;
}

void testEmissionAndDeterminism() {
  using namespace sysy::flat;
  Module module;
  Function* main = module.createFunction("main", typePool().i32());
  BasicBlock* entry = module.createBlock(main, "entry");
  Constant* one = module.getIntConst(typePool().i32(), 1);
  entry->addInst(createRet(one, {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string first;
  std::string second;
  std::string error;
  const bool ok = emitter.emit(module, first, error);
  expect(ok && error.empty(), "valid flat module emits");
  expect(first.find("define i32 @main()") != std::string::npos &&
             first.find("ret i32 1") != std::string::npos,
         "function and constant have LLVM spelling");
  expect(emitter.emit(module, second, error) && first == second,
         "repeated emission is deterministic");
}

void testFailureIsAtomic() {
  using namespace sysy::flat;
  Module module;
  Function* fn = module.createFunction("bad", typePool().voidTy());
  BasicBlock* entry = module.createBlock(fn, "entry");
  Instruction* shift = module.createInst(Opcode::Shl, typePool().i32(), {});
  module.ownInst(shift);
  entry->addInst(shift);
  entry->addInst(createRet(nullptr, {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) && !error.empty(),
         "malformed supported opcode is rejected");
  expect(output == "keep-existing-artifact", "failure leaves caller output unchanged");
}

void testTypedMemoryContract() {
  using namespace sysy::flat;
  Module module;
  GlobalVariable* global = module.createGlobal("float_slot", typePool().f32());
  Function* fn = module.createFunction("bad_load", typePool().voidTy());
  BasicBlock* entry = module.createBlock(fn, "entry");
  Instruction* load = module.createInst(Opcode::Load, typePool().i32(), {});
  load->addOperand(module.globalAddr(global));
  module.ownInst(load);
  entry->addInst(load);
  entry->addInst(createRet(nullptr, {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) &&
             error.find("load type does not match") != std::string::npos,
         "load result must match typed pointer element");
  expect(output == "keep-existing-artifact", "typed mismatch leaves output unchanged");
}

void testSparseGlobalBudget() {
  using namespace sysy::flat;
  Module module;
  GlobalVariable* global = module.createGlobal(
      "sparse", typePool().arrayOf(typePool().i32(), 1000001));
  global->setInitData({{0, 1}});

  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) &&
             error.find("aggregate emission budget") != std::string::npos,
         "oversized sparse nonzero global is rejected by resource budget");
  expect(output == "keep-existing-artifact", "global budget failure is atomic");
}

void testIntrinsicSignature() {
  using namespace sysy::flat;
  Module module;
  GlobalVariable* storage = module.createGlobal("storage", typePool().i32());
  Function* fn = module.createFunction("bad_intrinsic", typePool().voidTy());
  BasicBlock* entry = module.createBlock(fn, "entry");
  Instruction* call = module.createInst(Opcode::LLVMMemCpy, typePool().i32(), {});
  call->setCallee("llvm.memcpy");
  call->addOperand(module.globalAddr(storage));
  call->addOperand(module.globalAddr(storage));
  call->addOperand(module.getIntConst(typePool().i64(), 0));
  call->addOperand(module.getIntConst(typePool().i32(), 1));
  module.ownInst(call);
  entry->addInst(call);
  entry->addInst(createRet(nullptr, {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) &&
             error.find("intrinsics must return void") != std::string::npos,
         "memory intrinsic must have void result");
  expect(output == "keep-existing-artifact", "bad intrinsic signature is atomic");
}

void testFPExtAbiType() {
  using namespace sysy::flat;
  Module module;
  GlobalVariable* format = module.createGlobal("format", typePool().arrayOf(typePool().i32(), 2));
  Function* fn = module.createFunction("promote", typePool().voidTy());
  BasicBlock* entry = module.createBlock(fn, "entry");
  Instruction* promote = module.createInst(Opcode::FPExt, typePool().f32(), {});
  promote->addOperand(module.getFloatConst(0x3f800000));
  module.ownInst(promote);
  Instruction* call = module.createInst(Opcode::Call, nullptr, {});
  call->setCallee("putf");
  call->addOperand(module.globalAddr(format));
  call->addOperand(promote);
  module.ownInst(call);
  entry->addInst(promote);
  entry->addInst(call);
  entry->addInst(createRet(nullptr, {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string output;
  std::string error;
  const bool ok = emitter.emit(module, output, error);
  if (!ok) std::printf("FPExt emitter diagnostic: %s\n", error.c_str());
  expect(ok && output.find("fpext float") != std::string::npos &&
             output.find("declare void @putf(ptr, ...)") != std::string::npos &&
             output.find("call void @putf(ptr ") != std::string::npos &&
             output.find(", double ") != std::string::npos,
         "fpext result keeps LLVM double type through the putf variadic ABI");
}

void testFPExtOutsideVariadicABIIsRejected() {
  using namespace sysy::flat;
  Module module;
  Function* fn = module.createFunction("bad_promotion", typePool().voidTy());
  BasicBlock* entry = module.createBlock(fn, "entry");
  Instruction* promote = module.createInst(Opcode::FPExt, typePool().f32(), {});
  promote->addOperand(module.getFloatConst(0x3f800000));
  module.ownInst(promote);
  Instruction* call = module.createInst(Opcode::Call, nullptr, {});
  call->setCallee("external_sink");
  call->addOperand(promote);
  module.ownInst(call);
  entry->addInst(promote);
  entry->addInst(call);
  entry->addInst(createRet(nullptr, {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) &&
             error.find("putf variadic argument") != std::string::npos &&
             output == "keep-existing-artifact",
         "fpext is rejected outside the supported variadic ABI");
}

void testVariadicRuntimeCannotBeDefinedLocally() {
  using namespace sysy::flat;
  Module module;
  Function* putf = module.createFunction("putf", typePool().voidTy());
  BasicBlock* entry = module.createBlock(putf, "entry");
  entry->addInst(createRet(nullptr, {}));
  module.rebuildCFG();
  module.rebuildUseDef();
  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) &&
             error.find("cannot be defined") != std::string::npos &&
             output == "keep-existing-artifact",
         "putf retains its variadic runtime ABI");
}

void testMalformedVariadicRuntimeDeclarationIsRejected() {
  using namespace sysy::flat;
  Module module;
  module.createFunction("putf", typePool().i32());
  LLVMTextEmitter emitter;
  std::string output = "keep-existing-artifact";
  std::string error;
  expect(!emitter.emit(module, output, error) &&
             error.find("runtime placeholder signature") != std::string::npos &&
             output == "keep-existing-artifact",
         "putf declaration cannot silently discard a conflicting fixed signature");
}

void testQuotedGlobalSymbolEscape() {
  using namespace sysy::flat;
  Module module;
  module.createGlobal("a\"b\\c", typePool().i32());
  Function* fn = module.createFunction("main", typePool().i32());
  BasicBlock* entry = module.createBlock(fn, "entry");
  entry->addInst(createRet(module.getIntConst(typePool().i32(), 0), {}));
  module.rebuildCFG();
  module.rebuildUseDef();

  LLVMTextEmitter emitter;
  std::string output;
  std::string error;
  expect(emitter.emit(module, output, error) &&
             output.find("@\"a\\22b\\5Cc\"") != std::string::npos,
         "quoted LLVM global names use hexadecimal byte escapes");
}

}  // namespace

int main() {
  testEmissionAndDeterminism();
  testFailureIsAtomic();
  testTypedMemoryContract();
  testSparseGlobalBudget();
  testIntrinsicSignature();
  testFPExtAbiType();
  testFPExtOutsideVariadicABIIsRejected();
  testVariadicRuntimeCannotBeDefinedLocally();
  testMalformedVariadicRuntimeDeclarationIsRejected();
  testQuotedGlobalSymbolEscape();
  return failures == 0 ? 0 : 1;
}
