// Focused F00 first-class Value and exact Use regression tests.
#include <cstdio>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "ir/Module.h"
#include "ir/IRMutator.h"
#include "support/Diagnostic.h"

using namespace sysy::flat;
namespace {
int failures=0;
void check(bool ok, const char* name) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}
bool rauw(IRMutator& mut, Value* from, Value* to) {
  return mut.replaceUses(from, to) != MutationResult::Rejected;
}
bool edge(const Value* v, const Instruction* u, size_t index) {
  size_t n=0;
  for (const Use& item:v->uses())
    if (item.user==u && item.operandIndex==index) ++n;
  return n==1;
}
Instruction* own(Module& m, BasicBlock* b, Instruction* i) {
  m.ownInst(i);
  b->addInst(i);
  return i;
}
void valuesAndRAUW() {
  static_assert(!std::is_base_of<Instruction, Argument>::value,
                "Argument must not be an Instruction");
  Module m;
  IRMutator mut(m);
  const Type* t=typePool().i32();
  const Type* i64=typePool().i64();
  Function* f=m.createFunction("values",t);
  BasicBlock* b=m.createBlock(f);
  Constant* c=m.getIntConst(t,1);
  Instruction* x=own(m,b,createBin(Opcode::Add,c,c,{}));
  Instruction* y=own(m,b,createBin(Opcode::Add,x,x,{}));
  Instruction* z=own(m,b,createBin(Opcode::Add,x,c,{}));
  check(x->useCount()==3 && edge(x,y,0) && edge(x,y,1) && edge(x,z,0),
        "repeated operand multiplicity and multiple users");
  Instruction* replacement=own(m,b,createBin(Opcode::Add,c,c,{}));
  check(rauw(mut,x,replacement) && x->useCount()==0 &&
        edge(replacement,y,0) && edge(replacement,y,1) &&
        edge(replacement,z,0), "RAUW exact repeated indexes");
  check(rauw(mut,replacement,replacement) &&
        replacement->useCount()==3, "self RAUW no duplicate use");
  check(!rauw(mut,replacement,nullptr) &&
        !rauw(mut,replacement,m.getIntConst(i64,1)) &&
        edge(replacement,y,0), "RAUW null and type mismatch atomic");
  y->setOperand(0,c);
  check(edge(c,y,0) && !edge(replacement,y,0) && edge(replacement,y,1),
        "setOperand removes exact old edge");
  y->setOperand(0,c);
  check(edge(c,y,0), "setOperand old==new no duplicate");
  y->setOperand(1,c);
  Instruction* ret=own(m,b,createRet(c,{}));
  check(edge(c,y,0) && edge(c,y,1) && edge(c,ret,0),
        "Constant has three exact use occurrences");
  check(rauw(mut,c,replacement) && !c->hasUses() &&
        edge(replacement,ret,0), "Constant RAUW to instruction");
  y->clearOperands();
  check(y->numOperands()==0 && !edge(replacement,y,0) &&
        !edge(replacement,y,1), "clearOperands removes all indexes");
  y->addOperand(nullptr);
  check(y->operand(0)==nullptr && y->numOperands()==1,
        "null placeholder allowed");
  y->setOperand(0,c);
  check(edge(c,y,0) && c->useCount()==1, "null placeholder fixup records edge");
  y->clearOperands();
  // Selftest-only invalid edge, to check RAUW preflight without unsafe public API.
  auto& reverse=const_cast<std::vector<Use>&>(replacement->uses());
  reverse.push_back(Use{ret,static_cast<size_t>(999)});
  const size_t oldCount=replacement->useCount();
  check(!rauw(mut,replacement,c) && replacement->useCount()==oldCount &&
        ret->operand(0)==replacement, "stale reverse-use RAUW failure is atomic");
  m.rebuildUseDef();
  check(replacement->useCount()==7 && edge(replacement,x,0) &&
        edge(replacement,x,1) && edge(replacement,z,0) &&
        edge(replacement,ret,0), "repair rebuildUseDef recovers indexed uses");
  auto& duplicate=const_cast<std::vector<Use>&>(replacement->uses());
  duplicate.push_back(Use{z,0});
  check(!rauw(mut,replacement,c) && z->operand(0)==replacement,
        "duplicate reverse edge causes atomic RAUW failure");
  m.rebuildUseDef();
  check(rauw(mut,replacement,c) && !replacement->hasUses() &&
        edge(c,ret,0) && edge(c,z,0), "RAUW Instruction to Constant");
  Constant* two=m.getIntConst(t,2);
  check(rauw(mut,c,two) && !c->hasUses() && edge(two,ret,0),
        "RAUW Constant to Constant");
}
void argumentsAndGlobals() {
  Module m;
  IRMutator mut(m);
  const Type* t=typePool().i32();
  Function* f=m.createFunction("arguments",t);
  BasicBlock* b=m.createBlock(f);
  Argument* arg=m.createArgument(t,sysy::SourceLoc(9,1));
  f->addParam(arg);
  check(arg->isArgument() && !arg->isInst() && arg->owner()==f &&
        arg->index()==0 && b->param(0)==arg, "Argument independent Value owner/index");
  Instruction* addi=own(m,b,createBin(Opcode::Add,arg,arg,{}));
  Instruction* ret=own(m,b,createRet(arg,{}));
  check(arg->useCount()==3 && edge(arg,addi,0) && edge(arg,addi,1) &&
        edge(arg,ret,0), "Argument duplicate uses and direct return");
  Constant* c=m.getIntConst(t,10);
  check(rauw(mut,arg,c) && !arg->hasUses() &&
        c->useCount()==3, "Argument RAUW to canonical Constant");
  GlobalVariable* gv=m.createGlobal("glob",t);
  GlobalAddr* ga=m.globalAddr(gv);
  Instruction* load=own(m,b,createLoad(t,ga,{}));
  Instruction* store=own(m,b,createStore(t,c,ga,{}));
  check(ga->useCount()==2 && edge(ga,load,0) && edge(ga,store,1),
        "GlobalAddr exact use indexes");
  Instruction* ptr=own(m,b,createAlloca(t,{}));
  check(rauw(mut,ga,ptr) && !ga->hasUses() &&
        edge(ptr,load,0) && edge(ptr,store,1),
        "GlobalAddr RAUW to pointer value");
  BlockAddr* ba=m.createBlockAddr(typePool().ptrTo(t),b);
  Instruction* baUse=own(m,b,createLoad(t,ba,{}));
  check(ba->isBlockAddr() && edge(ba,baUse,0),
        "BlockAddr tracks a structural operand use");
  check(rauw(mut,ba,ptr) && !ba->hasUses() &&
        edge(ptr,baUse,0), "BlockAddr RAUW to typed pointer");
  m.rebuildUseDef();
  check(ptr->useCount()==3 && c->useCount()==4,
        "repair reconstructs constant and instruction uses");
  Function* foreign=m.createFunction("foreign",t);
  BasicBlock* fb=m.createBlock(foreign);
  foreign->addParam(arg);
  own(m,fb,createRet(arg,{}));
  m.rebuildCFG();
  m.rebuildUseDef();
  check(!verifyFlatModule(m).empty(), "foreign Argument rejected");
}
void unreferencedReverseEdge() {
  Module m;
  const Type* i32=typePool().i32();
  Function* f=m.createFunction("orphan",i32);
  BasicBlock* b=m.createBlock(f);
  Constant* one=m.getIntConst(i32,1);
  Instruction* ret=own(m,b,createRet(one,{}));
  Constant* unused=m.getIntConst(i32,2);
  m.rebuildCFG();
  check(verifyFlatModule(m).empty(), "valid unreferenced Constant accepted");
  const_cast<std::vector<Use>&>(unused->uses()).push_back(Use{ret,0});
  check(!verifyFlatModule(m).empty(),
        "verifier catches stale reverse Use on unreferenced Constant");
  m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "repair clears unreferenced stale Use");
}
void canonicalAndRoundTrip() {
  Module m;
  const Type* i32=typePool().i32();
  const Type* i64=typePool().i64();
  check(m.getIntConst(i32,1)==m.getIntConst(i32,0x100000001LL),
        "i32 raw bits normalize low32");
  check(m.getIntConst(i32,1)!=m.getIntConst(i64,1),
        "i32 i64 distinct constants");
  check(m.getIntConst(i32,0xffffffffLL)->intVal()==-1,
        "i32 normalized signed readback");
  check(m.getFloatConst(0u)!=m.getFloatConst(0x80000000u),
        "signed float zero stays distinct");
  check(m.getFloatConst(0x7fc00001u)==m.getFloatConst(0x7fc00001u),
        "same NaN payload shares pointer");
  check(m.getFloatConst(0x7fc00001u)!=m.getFloatConst(0x7fc00002u),
        "different NaN payloads are distinct");
  GlobalVariable* gv=m.createGlobal("glob",i32);
  Constant* one=m.getIntConst(i32,1);
  for (int index=0;index<2;++index) {
    Function* f=m.createFunction(index?"other":"first",i32);
    BasicBlock* block=m.createBlock(f);
    Argument* a=m.createArgument(i32,{});
    f->addParam(a);
    f->rememberConstantLoc(one,sysy::SourceLoc(index+10,1));
    own(m,block,createBin(Opcode::Add,one,one,{}));
    own(m,block,createBin(Opcode::Add,a,one,{}));
    own(m,block,createLoad(i32,m.globalAddr(gv),{}));
    Constant* pos=m.getFloatConst(0u);
    Constant* neg=m.getFloatConst(0x80000000u);
    f->rememberConstantLoc(pos,sysy::SourceLoc(index+20,1));
    f->rememberConstantLoc(neg,sysy::SourceLoc(index+30,1));
    own(m,block,createBin(Opcode::FAdd,pos,neg,{}));
    own(m,block,createCast(Opcode::Trunc,m.getIntConst(i64,1),{}));
    own(m,block,createRet(one,{}));
  }
  m.rebuildCFG();
  m.rebuildUseDef();
  std::string first=dumpModule(m);
  check(first==dumpModule(m), "dump twice byte-identical");
  check(first.find("i32 1 @line 10")!=std::string::npos &&
        first.find("i32 1 @line 11")!=std::string::npos,
        "canonical constant preserves per-function source line");
  sysy::DiagnosticEngine diags;
  Module* parsed=parseFlatModule(first,diags);
  check(parsed!=nullptr, "FlatReader reads shared canonical constants");
  if(parsed) {
    check(dumpModule(*parsed)==first, "dump reader dump byte-identical");
    delete parsed;
  }
  m.clear();
  m.clear();
  check(m.functions().empty() && m.globals().empty(),
        "Module clear repeated safely");
}
}
int main() {
  valuesAndRAUW();
  argumentsAndGlobals();
  canonicalAndRoundTrip();
  unreferencedReverseEdge();
  std::printf("VALUE_USE_FAILURES=%d\n",failures);
  return failures?1:0;
}
