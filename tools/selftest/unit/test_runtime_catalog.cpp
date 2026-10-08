#include "frontend/RuntimeLib.h"
#include "support/RuntimeABI.h"
#include "support/RuntimeLib.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
  using namespace sysy;
  TypeContext& types = typeContext();
  const Type* i = types.intType();
  const Type* f = types.floatType();
  const Type* v = types.voidType();
  const Type* ia = types.arrayOf(i, kUnknownDim);
  const Type* fa = types.arrayOf(f, kUnknownDim);
  struct Expected {
    const char* name;
    const Type* result;
    std::vector<const Type*> params;
    bool uncallable = false;
    runtime::Behavior behavior = runtime::Behavior::Unsupported;
  };
  // 独立的公开签名预期，验证适配后仍使用前端驻留类型，顺序不漂移。
  const std::vector<Expected> expected{
      {"getint", i, {}, false, runtime::Behavior::ReadInt},
      {"getch", i, {}, false, runtime::Behavior::ReadChar},
      {"getarray", i, {ia}, false, runtime::Behavior::ReadIntArray},
      {"getfloat", f, {}, false, runtime::Behavior::ReadFloat},
      {"getfarray", i, {fa}, false, runtime::Behavior::ReadFloatArray},
      {"putint", v, {i}, false, runtime::Behavior::WriteInt},
      {"putch", v, {i}, false, runtime::Behavior::WriteChar},
      {"putarray", v, {i, ia}, false, runtime::Behavior::WriteIntArray},
      {"putfloat", v, {f}, false, runtime::Behavior::WriteFloat},
      {"putfarray", v, {i, fa}, false, runtime::Behavior::WriteFloatArray},
      {"putf", v, {}, true, runtime::Behavior::Unsupported},
      {"starttime", v, {}, false, runtime::Behavior::NoOp},
      {"stoptime", v, {}, false, runtime::Behavior::NoOp}};
  int failures = 0;
  auto check = [&failures](bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
  };
  const auto& actual = runtimeFunctions();
  check(actual.size() == expected.size(), "catalog size");
  check(runtime::kSignatures.size() == expected.size(), "neutral catalog size");
  for (size_t n = 0; n < expected.size(); ++n) {
    const auto& e = expected[n];
    const FuncSig* signature = lookupRuntimeFunc(e.name);
    check(signature != nullptr, e.name);
    if (!signature) continue;
    check(signature->ret == e.result && signature->params == e.params &&
              signature->uncallable == e.uncallable, std::string(e.name) + " types");
    check(n < actual.size() && std::string(actual[n].name) == e.name &&
              actual[n].sig == signature, std::string(e.name) + " order/stable identity");
    const auto* neutral = runtime::lookup(e.name);
    check(neutral && (neutral->result == runtime::Kind::Void) == isVoid(e.result),
          std::string(e.name) + " IR result arity");
    check(neutral && neutral->behavior == e.behavior,
          std::string(e.name) + " reference execution behavior");
  }
  for (const char* absent : {"unknown", "_sysy_starttime", "_sysy_stoptime"}) {
    check(runtime::lookup(absent) == nullptr && lookupRuntimeFunc(absent) == nullptr,
          std::string(absent) + " is not a source-visible runtime function");
  }
  const auto* start = runtime::lookupAbi("starttime");
  const auto* stop = runtime::lookupAbi("stoptime");
  check(start && start->targetName == "_sysy_starttime" && start->appendCallLine,
        "starttime target ABI mapping");
  check(stop && stop->targetName == "_sysy_stoptime" && stop->appendCallLine,
        "stoptime target ABI mapping");
  check(runtime::lookupAbi("getint") == nullptr, "ordinary runtime has no ABI remapping");
  check(runtime::lookupAbi("_sysy_starttime") == nullptr,
        "target-only ABI symbol not accepted as source name");
  std::cout << "Runtime catalog: 13 signatures; failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
