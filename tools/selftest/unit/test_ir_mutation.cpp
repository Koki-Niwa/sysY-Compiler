// F01 focused mutation, lifecycle, CFG/phi, revision and atomicity tests.
#include <cstdio>
#include <algorithm>
#include <string>
#include "ir/CFGEditor.h"
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "ir/IRMutator.h"

using namespace sysy::flat;
namespace {
int failures = 0;
void check(bool ok, const char* name) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}
bool use(const Value* v, const Instruction* inst, size_t index) {
  return std::count_if(v->uses().begin(), v->uses().end(), [=](const Use& u) {
    return u.user == inst && u.operandIndex == index;
  }) == 1;
}
Instruction* add(Module& m, BasicBlock* b, Instruction* inst) {
  m.ownInst(inst);
  b->addInst(inst);
  return inst;
}
void valid(const Module& m, const char* name) {
  const auto errors = verifyFlatModule(m);
  if (!errors.empty()) std::fprintf(stderr,"%s\n",formatViolations(errors).c_str());
  check(errors.empty(),name);
}

void operandsAndLifecycle() {
  Module m;
  const Type* t = typePool().i32();
  Function* f = m.createFunction("core",t);
  BasicBlock* b = m.createBlock(f);
  Constant* one=m.getIntConst(t,1);
  Constant* two=m.getIntConst(t,2);
  Constant* wide=m.getIntConst(typePool().i64(),1);
  Instruction* x=add(m,b,createBin(Opcode::Add,one,one,{}));
  Instruction* y=add(m,b,createBin(Opcode::Add,x,x,{}));
  Instruction* ret=add(m,b,createRet(y,{}));
  IRMutator mut(m);
  check(m.revision().any==0 && f->revision().any==0 && !mut.summary().changed(),
        "builder keeps initial revision at zero");
  check(mut.rewriteOperand(x,0,two)==MutationResult::Applied &&
        m.revision().dataFlow==1 && m.revision().cfg==0 &&
        use(two,x,0) && !use(one,x,0),
        "setOperand precise edge and DataFlow revision");
  auto rev=m.revision().any;
  check(mut.rewriteOperand(x,0,two)==MutationResult::NoChange &&
        mut.rewriteOperand(x,0,wide)==MutationResult::Rejected &&
        mut.replaceUses(one,one)==MutationResult::NoChange &&
        m.revision().any==rev && use(two,x,0),
        "NoChange and type reject atomic revision");
  check(mut.replaceUses(x,one)==MutationResult::Applied &&
        !x->hasUses() && use(one,y,0) && use(one,y,1),
        "repeated-operand RAUW exact indices");
  check(mut.eraseInstruction(y)==MutationResult::Rejected && m.isLiveInstruction(y) &&
        mut.eraseInstruction(ret)==MutationResult::Rejected,
        "used and terminator erase rejected");
  check(mut.replaceAndErase(y,two)==MutationResult::Applied &&
        m.isTombstone(y) && y->parent()==nullptr && !m.isLiveInstruction(y) &&
        y->numOperands()==0 && y->numSuccs()==0 && use(two,ret,0),
        "replaceAndErase tombstone and reverse uses");
  rev=m.revision().any;
  check(mut.insertInstruction(b,0,y)==MutationResult::Rejected &&
        m.revision().any==rev, "tombstone reinsert rejected without bump");
  check(mut.eraseInstruction(x)==MutationResult::Applied && m.isTombstone(x),
        "unused erase clears owned/live distinction");
  Instruction* inserted=createBin(Opcode::Add,one,two,{});
  check(mut.insertInstruction(b,0,inserted)==MutationResult::Applied &&
        m.ownsInstruction(inserted) && m.isLiveInstruction(inserted) &&
        inserted->lifecycle()==InstructionLifecycle::Live,
        "detached instruction insert becomes live");
  m.rebuildCFG(); // fixture finalization, not optimization
  valid(m,"post mutation verifier without rebuild repair");
  check(mut.summary().operations>=5 &&
        mut.summary().affectedFunctions.count(f)==1 &&
        hasMutation(mut.summary().kinds,MutationKind::DataFlow),
        "summary derives only from committed operations");
}

void crossFunctionAndScratch() {
  Module m;
  const Type* t=typePool().i32();
  Constant* a=m.getIntConst(t,5), *b=m.getIntConst(t,7);
  Function* f=m.createFunction("A",t);
  Function* g=m.createFunction("B",t);
  BasicBlock* x=m.createBlock(f), *y=m.createBlock(g);
  Instruction* r1=add(m,x,createRet(a,{}));
  Instruction* r2=add(m,y,createRet(a,{}));
  IRMutator mut(m);
  Instruction* scratch=createBin(Opcode::Add,a,a,{});
  check(mut.rewriteOperand(scratch,1,b)==MutationResult::Applied &&
        m.revision().any==0 && !mut.summary().changed(),
        "detached scratch edit has no revision");
  check(mut.discardDetached({scratch})==MutationResult::Applied &&
        m.revision().any==0, "discard detached scratch without live revision");
  check(mut.replaceUses(a,b)==MutationResult::Applied &&
        m.revision().any==1 && f->revision().any==1 &&
        g->revision().any==1 && use(b,r1,0) && use(b,r2,0) &&
        mut.summary().affectedFunctions.size()==2,
        "canonical Constant RAUW bumps both Functions and Module");
  m.rebuildCFG();
  valid(m,"cross-function value RAUW verifier");
  const auto previous=m.revision().any;
  m.clear();
  check(m.revision().any==previous,
        "Module clear does not roll back tracked revision");
}

void categories() {
  Module m;
  const Type* t=typePool().i32();
  Function* f=m.createFunction("categories",t);
  BasicBlock* b=m.createBlock(f);
  Constant* one=m.getIntConst(t,1), *two=m.getIntConst(t,2);
  Instruction* slot=add(m,b,createAlloca(t,{}));
  Instruction* store=add(m,b,createStore(t,one,slot,{}));
  Instruction* load=add(m,b,createLoad(t,slot,{}));
  Instruction* call=add(m,b,createCall("putint",nullptr,{one},{}));
  add(m,b,createRet(load,{}));
  IRMutator mut(m);
  check(mut.rewriteOperand(store,0,two)==MutationResult::Applied &&
        m.revision().dataFlow==1 && m.revision().memory==1 &&
        m.revision().calls==0,"store edit DataFlow+Memory");
  check(mut.rewriteOperand(call,0,two)==MutationResult::Applied &&
        m.revision().dataFlow==2 && m.revision().memory==2 &&
        m.revision().calls==1, "call argument edit DataFlow+Calls+Memory");
  check(mut.rewriteOperand(load,0,slot)==MutationResult::NoChange &&
        m.revision().any==2,"unchanged pointer operand no bump");
  m.rebuildCFG();
  valid(m,"memory/call mutation verifier");
}

void insertionAndPhi() {
  Module m;
  const Type* t=typePool().i32();
  Function* f=m.createFunction("phi",t);
  BasicBlock* a=m.createBlock(f);
  BasicBlock* b=m.createBlock(f);
  Constant* one=m.getIntConst(t,1), *two=m.getIntConst(t,2);
  add(m,a,createBr(b,{}));
  Instruction* ret=add(m,b,createRet(one,{}));
  m.rebuildCFG();
  IRMutator mut(m);
  CFGEditor cfg(mut);
  Instruction* phi=createPhi(t,{one},{a},{});
  check(cfg.insertPhi(b,phi)==MutationResult::Applied &&
        m.isLiveInstruction(phi) && m.ownsInstruction(phi),
        "insert phi with exact predecessor set");
  check(cfg.rewritePhiValue(phi,a,two)==MutationResult::Applied &&
        phi->operand(0)==two && use(two,phi,0),
        "phi value rewrite by predecessor");
  auto rev=m.revision().any;
  check(cfg.rewritePhiValue(phi,a,two)==MutationResult::NoChange &&
        cfg.rewritePhiValue(phi,a,m.getIntConst(typePool().i64(),2))==
          MutationResult::Rejected && m.revision().any==rev,
        "phi NoChange and wrong type rejected");
  check(mut.rewriteOperand(ret,0,phi)==MutationResult::Applied,
        "phi result referenced by return");
  valid(m,"insert/rewrite phi verifier without rebuild");
  check(cfg.erasePhi(phi)==MutationResult::Rejected,"used phi cannot be erased");
  check(mut.rewriteOperand(ret,0,two)==MutationResult::Applied &&
        cfg.erasePhi(phi)==MutationResult::Applied && m.isTombstone(phi),
        "erase unused phi via CFGEditor");
  valid(m,"erase phi verifier");
}

void redirectWithPhi() {
  Module m;
  const Type* t=typePool().i32();
  Function* f=m.createFunction("redirect",t);
  BasicBlock* a=m.createBlock(f);
  BasicBlock* b=m.createBlock(f);
  BasicBlock* c=m.createBlock(f);
  BasicBlock* d=m.createBlock(f);
  Constant* one=m.getIntConst(t,1), *two=m.getIntConst(t,2);
  add(m,a,createCondBr(one,b,b,{}));
  add(m,b,createRet(one,{}));
  Instruction* phi=add(m,c,createPhi(t,{one},{d},{}));
  add(m,c,createRet(phi,{}));
  add(m,d,createBr(c,{}));
  m.rebuildCFG();
  IRMutator mut(m);
  CFGEditor cfg(mut);
  auto before=dumpModule(m);
  auto revision=m.revision().any;
  check(cfg.redirectEdge(a,0,c)==MutationResult::Rejected &&
        dumpModule(m)==before && m.revision().any==revision &&
        b->preds().size()==1 && c->preds().size()==1,
        "missing new phi value reject is dump/CFG/revision atomic");
  check(cfg.redirectEdge(a,0,c,{two})==MutationResult::Applied &&
        a->terminator()->succ(0)==c &&
        b->preds().size()==1 && b->pred(0)==a &&
        c->preds().size()==2 && c->pred(0)==a && c->pred(1)==d &&
        phi->succ(0)==a && phi->operand(0)==two &&
        phi->succ(1)==d && phi->operand(1)==one &&
        use(two,phi,0) && use(one,phi,1),
        "duplicate-successor redirect adds pred and reorders phi/use indexes");
  revision=m.revision().any;
  check(cfg.redirectEdge(a,0,c)==MutationResult::NoChange &&
        m.revision().any==revision,
        "redirect unchanged target no bump");
  valid(m,"redirect with phi and duplicate successor verifier");
}

void redirectRemovePhi() {
  Module m;
  const Type* t=typePool().i32();
  Function* f=m.createFunction("remove_phi_incoming",t);
  BasicBlock* a=m.createBlock(f);
  BasicBlock* b=m.createBlock(f);
  BasicBlock* c=m.createBlock(f);
  BasicBlock* d=m.createBlock(f);
  Constant* one=m.getIntConst(t,1), *two=m.getIntConst(t,2);
  add(m,a,createBr(b,{}));
  Instruction* phi=add(m,b,createPhi(t,{one,two},{a,d},{}));
  add(m,b,createRet(phi,{}));
  add(m,c,createRet(one,{}));
  add(m,d,createBr(b,{}));
  m.rebuildCFG();
  IRMutator mut(m);
  CFGEditor cfg(mut);
  check(cfg.redirectEdge(a,0,c)==MutationResult::Applied &&
        b->numPreds()==1 && b->pred(0)==d &&
        phi->numOperands()==1 && phi->succ(0)==d &&
        phi->operand(0)==two && !use(one,phi,0),
        "redirect removes obsolete old-target phi incoming atomically");
  valid(m,"remove old phi incoming verifier");
}

void splitDuplicate(bool duplicate) {
  Module m;
  const Type* t=typePool().i32();
  Function* f=m.createFunction(duplicate?"split_dup":"split",t);
  BasicBlock* a=m.createBlock(f);
  BasicBlock* b=m.createBlock(f);
  Constant* one=m.getIntConst(t,1);
  if (duplicate) add(m,a,createCondBr(one,b,b,{}));
  else add(m,a,createBr(b,{}));
  Instruction* phi=add(m,b,createPhi(t,{one},{a},{}));
  add(m,b,createRet(phi,{}));
  m.rebuildCFG();
  IRMutator mut(m);
  CFGEditor cfg(mut);
  auto result=cfg.splitEdge(a,0);
  BasicBlock* mid=result.block;
  bool ok=result.result==MutationResult::Applied && mid!=nullptr &&
      mid->parent()==f && a->terminator()->succ(0)==mid &&
      mid->terminator()!=nullptr && mid->terminator()->succ(0)==b &&
      mid->numPreds()==1 && mid->pred(0)==a;
  if (duplicate) {
    ok=ok && a->terminator()->succ(1)==b &&
       b->numPreds()==2 && b->pred(0)==a && b->pred(1)==mid &&
       phi->numOperands()==2 && phi->succ(0)==a &&
       phi->succ(1)==mid && phi->operand(0)==one &&
       phi->operand(1)==one && use(one,phi,0) && use(one,phi,1);
  } else {
    ok=ok && b->numPreds()==1 && b->pred(0)==mid &&
       phi->numOperands()==1 && phi->succ(0)==mid &&
       phi->operand(0)==one && use(one,phi,0);
  }
  check(ok,duplicate?"split duplicate edge clones phi incoming":
                     "split simple edge rekeys phi incoming");
  check(hasMutation(mut.summary().kinds,MutationKind::CFG) &&
        hasMutation(mut.summary().kinds,MutationKind::DataFlow) &&
        f->revision().any==1,"split atomic mutation summary/revision");
  valid(m,duplicate?"duplicate split verifier":"simple split verifier");
}
void verifierLifecycles() {
  Module m;
  const Type* t=typePool().i32();
  Function* f=m.createFunction("live_audit",t);
  BasicBlock* block=m.createBlock(f);
  Constant* one=m.getIntConst(t,1);
  Instruction* unused=add(m,block,createBin(Opcode::Add,one,one,{}));
  add(m,block,createRet(one,{}));
  m.rebuildCFG();
  IRMutator mut(m);
  check(mut.eraseInstruction(unused)==MutationResult::Applied,
        "prepare tombstone fixture");
  Instruction* detached=createInst(Opcode::Add,t,{});
  m.ownInst(detached);
  check(verifyFlatModule(m).empty(),
        "owned detached instruction without residue is valid");
  detached->addOperand(one);
  detached->addOperand(one);
  check(!verifyFlatModule(m).empty(),
        "detached owned reverse-use residue rejected");
  detached->clearOperands();
  check(verifyFlatModule(m).empty(),
        "removing detached residue restores validity");
  unused->addOperand(one);
  check(!verifyFlatModule(m).empty(),
        "tombstone with operands rejected");
  unused->clearOperands();
  check(verifyFlatModule(m).empty(),
        "cleared tombstone accepted");
  Instruction* ret=block->terminator();
  m.rawMarkDetached(ret);  // intentionally corrupt registry, not an optimizer mutation
  check(!verifyFlatModule(m).empty(),
        "missing live registry entry rejected");
  m.rawMarkLive(ret);
  valid(m,"live registry recovery verifier");
}

void provenanceMutations() {
  const Type* t = typePool().i32();
  Module source, target;
  Function* sf=source.createFunction("source",t);
  BasicBlock* sb=source.createBlock(sf);
  Constant* foreign=source.getIntConst(t,77);
  Argument* foreignArg=source.createArgument(t,{});
  sf->addParam(foreignArg);
  GlobalAddr* foreignGlobal=source.globalAddr(source.createGlobal("x",t));
  BlockAddr* foreignBlock=source.createBlockAddr(typePool().ptrTo(t),sb);
  Instruction* owned=createBin(Opcode::Add,foreign,foreign,{});
  check(source.ownInst(owned) && owned->ownerModule()==&source,
        "source adopts Detached instruction exactly once");
  check(!target.ownInst(owned) && source.ownsInstruction(owned) &&
        !target.ownsInstruction(owned), "foreign ownInst fails closed");

  Function* f=target.createFunction("target",t);
  Function* other=target.createFunction("other",t);
  BasicBlock* b=target.createBlock(f);
  BasicBlock* ob=target.createBlock(other);
  Constant* local=target.getIntConst(t,77);
  Constant* next=target.getIntConst(t,88);
  Argument* sameModuleArg=target.createArgument(t,{});
  other->addParam(sameModuleArg);
  Instruction* cross=add(target,ob,createBin(Opcode::Add,local,local,{}));
  add(target,ob,createRet(local,{}));
  Instruction* def=add(target,b,createBin(Opcode::Add,local,local,{}));
  Instruction* user=add(target,b,createBin(Opcode::Add,def,local,{}));
  GlobalAddr* localGlobal=target.globalAddr(target.createGlobal("local",t));
  Instruction* load=add(target,b,createLoad(t,localGlobal,{}));
  Instruction* ret=add(target,b,createRet(user,{}));
  (void)load;
  target.rebuildCFG();
  valid(target,"provenance initial target");
  IRMutator mut(target);
  const std::string before=dumpModule(target);
  auto mr=target.revision().any, fr=f->revision().any, gr=other->revision().any;
  check(mut.insertInstruction(b,0,owned)==MutationResult::Rejected &&
        source.ownsInstruction(owned) && !target.ownsInstruction(owned) &&
        !target.isLiveInstruction(owned) && owned->parent()==nullptr &&
        dumpModule(target)==before && target.revision().any==mr,
        "foreign-owned Detached insert rejects atomically");
  check(mut.discardDetached({owned})==MutationResult::Rejected &&
        owned->ownerModule()==&source && source.ownsInstruction(owned) &&
        target.revision().any==mr, "foreign discardDetached preserves source lifetime");
  auto stable=[&]() {
    return target.revision().any==mr && f->revision().any==fr &&
      other->revision().any==gr && dumpModule(target)==before &&
      use(def,user,0) && use(local,user,1) && use(user,ret,0);
  };
  check(mut.rewriteOperand(user,1,foreign)==MutationResult::Rejected &&
        stable(), "foreign Constant rewrite rejected without changes");
  check(mut.rewriteOperand(user,1,foreignArg)==MutationResult::Rejected &&
        stable(), "foreign Argument rewrite rejected atomically");
  check(mut.rewriteOperand(user,1,sameModuleArg)==MutationResult::Rejected &&
        stable(), "other Function Argument rewrite rejected atomically");
  check(mut.rewriteOperand(user,1,cross)==MutationResult::Rejected &&
        stable(), "other Function Instruction rewrite rejected atomically");
  check(mut.rewriteOperand(user,1,owned)==MutationResult::Rejected &&
        stable(), "foreign-owned Instruction rewrite rejected atomically");
  check(mut.rewriteOperand(load,0,foreignGlobal)==MutationResult::Rejected &&
        stable(), "foreign GlobalAddr rewrite rejected atomically");
  check(mut.rewriteOperand(load,0,foreignBlock)==MutationResult::Rejected &&
        stable(), "foreign BlockAddr rewrite rejected atomically");
  check(mut.replaceUses(local,foreign)==MutationResult::Rejected && stable(),
        "foreign Constant replaceUses rejects all users");
  check(mut.replaceUses(local,sameModuleArg)==MutationResult::Rejected && stable(),
        "multi-function replaceUses rejects partial rewrite");
  check(mut.replaceAndErase(def,foreign)==MutationResult::Rejected &&
        target.isLiveInstruction(def) && stable(),
        "foreign Constant replaceAndErase preserves old instruction");
  check(mut.replaceAndErase(def,cross)==MutationResult::Rejected &&
        target.isLiveInstruction(def) && stable(),
        "other Function replaceAndErase rejected atomically");
  check(target.globalAddr(source.globals().front())==nullptr &&
        target.createBlockAddr(typePool().ptrTo(t),sb)==nullptr,
        "cross-module address factories fail closed");
  size_t paramsBefore=f->numParams();
  f->addParam(foreignArg);
  f->addParam(sameModuleArg);
  check(f->numParams()==paramsBefore && foreignArg->owner()==sf &&
        sameModuleArg->owner()==other, "Function addParam refuses foreign/reused Arguments");
  check(source.ownInst(owned) && source.ownedInstructions().size()==1,
        "same-module ownInst remains idempotent");
  check(stable() && verifyFlatModule(target).empty(), "rejected provenance mutations keep valid IR");
  (void)next;
}

void provenanceCFG() {
  const Type* t=typePool().i32();
  Module m, foreign;
  Function* f=m.createFunction("cfg",t);
  Function* other=m.createFunction("other",t);
  BasicBlock* a=m.createBlock(f), *b=m.createBlock(f);
  BasicBlock* c=m.createBlock(f), *d=m.createBlock(f);
  BasicBlock* ob=m.createBlock(other);
  Constant* one=m.getIntConst(t,1);
  Constant* outsider=foreign.getIntConst(t,1);
  Argument* foreignArg=foreign.createArgument(t,{});
  Function* foreignFn=foreign.createFunction("foreign",t);
  foreign.createBlock(foreignFn);
  foreignFn->addParam(foreignArg);
  Argument* otherArg=m.createArgument(t,{});
  other->addParam(otherArg);
  Instruction* otherDef=add(m,ob,createBin(Opcode::Add,one,one,{}));
  add(m,ob,createRet(one,{}));
  add(m,a,createBr(b,{}));
  add(m,b,createRet(one,{}));
  add(m,c,createRet(one,{}));
  add(m,d,createBr(c,{}));
  m.rebuildCFG();
  IRMutator mut(m);
  CFGEditor cfg(mut);
  const std::string before=dumpModule(m);
  auto rev=m.revision().any, frev=f->revision().any;
  auto stable=[&](){return m.revision().any==rev && f->revision().any==frev &&
                               dumpModule(m)==before;};
  auto checkPhi=[&](Value* value, const char* name) {
    Instruction* phi=createPhi(t,{value},{a},{});
    auto result=cfg.insertPhi(b,phi);
    check(result==MutationResult::Rejected && stable() &&
          !m.ownsInstruction(phi),name);
    delete phi;
  };
  checkPhi(outsider,"insertPhi rejects foreign Constant");
  checkPhi(foreignArg,"insertPhi rejects foreign Argument");
  checkPhi(otherArg,"insertPhi rejects same-module other Function Argument");
  checkPhi(otherDef,"insertPhi rejects same-module other Function Instruction");
  Instruction* ownedPhi=createPhi(t,{one},{a},{});
  foreign.ownInst(ownedPhi);
  check(cfg.insertPhi(b,ownedPhi)==MutationResult::Rejected &&
        foreign.ownsInstruction(ownedPhi) && !m.ownsInstruction(ownedPhi) &&
        stable(), "insertPhi rejects foreign-owned detached Phi");
  check(cfg.redirectEdge(a,0,c,{outsider})==MutationResult::Rejected &&
        stable(),"redirectEdge rejects foreign Constant atomically");
  check(cfg.redirectEdge(a,0,c,{otherArg})==MutationResult::Rejected &&
        stable(),"redirectEdge rejects cross-function Argument atomically");
  check(cfg.redirectEdge(a,0,c,{otherDef})==MutationResult::Rejected &&
        stable(),"redirectEdge rejects cross-function Instruction atomically");
  // A target with a Phi makes redirectEdge exercise incoming preflight.
  Instruction* phi=add(m,c,createPhi(t,{one},{d},{}));
  c->at(0); // Existing ret must follow the Phi for canonical ordering.
  c->removeInst(phi);
  c->insertInst(0,phi);
  m.rebuildCFG();
  const std::string withPhi=dumpModule(m);
  rev=m.revision().any;
  check(cfg.redirectEdge(a,0,c,{outsider})==MutationResult::Rejected &&
        dumpModule(m)==withPhi && m.revision().any==rev,
        "redirectEdge foreign Phi incoming rejected with no CFG edits");
  check(cfg.rewritePhiValue(phi,d,outsider)==MutationResult::Rejected &&
        dumpModule(m)==withPhi && m.revision().any==rev,
        "rewritePhiValue foreign Constant rejected");
  check(cfg.rewritePhiValue(phi,d,foreignArg)==MutationResult::Rejected &&
        dumpModule(m)==withPhi && m.revision().any==rev,
        "rewritePhiValue foreign Argument rejected");
}
} // namespace

int main() {
  operandsAndLifecycle();
  provenanceMutations();
  provenanceCFG();
  verifierLifecycles();
  crossFunctionAndScratch();
  categories();
  insertionAndPhi();
  redirectWithPhi();
  redirectRemovePhi();
  splitDuplicate(false);
  splitDuplicate(true);
  std::printf("F01 mutation tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
  return failures?1:0;
}
