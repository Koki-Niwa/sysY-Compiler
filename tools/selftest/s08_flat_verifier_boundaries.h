#ifndef SYSY_SELFTEST_S08_FLAT_VERIFIER_BOUNDARIES_H
#define SYSY_SELFTEST_S08_FLAT_VERIFIER_BOUNDARIES_H

// Focused public-boundary fixtures added after the original S08 matrix.  This
// header is included inside the matrix's anonymous namespace and intentionally
// reuses its small construction helpers.
std::unique_ptr<Module> badPredOrder() {
  std::unique_ptr<Module> m = validBranch();
  Function* f = m->function(0);
  BasicBlock* join = f->at(3);
  join->clearPreds();
  join->addPred(f->at(2));
  join->addPred(f->at(1));
  return m;
}

std::unique_ptr<Module> badNonTerminatorSuccessor() {
  std::unique_ptr<Module> m = validCall();
  Function* f = m->function(0);
  f->entry()->at(0)->addSucc(f->entry());
  return m;
}

std::unique_ptr<Module> badForeignGlobalAddr() {
  static GlobalVariable foreign("foreign_global", typePool().i32());
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_foreign_global_addr");
  GlobalAddr* address = m->globalAddr(&foreign);
  add(f->entry(), own(*m, createCall("sink", nullptr, {address}, L)));
  add(f->entry(), own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badRetypedGlobalAddr() {
  std::unique_ptr<Module> m(new Module());
  GlobalVariable* global = m->createGlobal("owned_global", typePool().i32());
  GlobalAddr* address = m->globalAddr(global);
  address->setType(typePool().ptrTo(typePool().f32()));
  Function* f = fn(*m, "bad_retyped_global_addr");
  add(f->entry(), own(*m, createCall("sink", nullptr, {address}, L)));
  add(f->entry(), own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

#endif  // SYSY_SELFTEST_S08_FLAT_VERIFIER_BOUNDARIES_H
