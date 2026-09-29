// ============================================================================
// RuntimeLib.cpp —— 运行时库签名表的实现（13 个函数，见 RuntimeLib.h 的头注）
//
// 全部 13 个（顺序 = prompt §3.4 的表顺序，也是 --emit=sema 的打印顺序）：
//
//   int   getint();          int   getch();        int   getarray(int a[]);
//   float getfloat();        int   getfarray(float a[]);
//   void  putint(int);       void  putch(int);     void  putarray(int n, int a[]);
//   void  putfloat(float);   void  putfarray(int n, float a[]);
//   void  putf(char a[], ...);                    // 变参，SysY 侧不可调用
//   void  starttime();       void  stoptime();    // sylib.h 里是宏，见头注
// ============================================================================
#include "frontend/RuntimeLib.h"
#include "support/RuntimeLib.h"

namespace sysy {
namespace {

// ⚠️ **必须**用全局唯一的那个类型工厂（Type.h 的 typeContext()）：
//    用户代码里的类型与运行时库形参的类型要能**指针相等**地比较
//    （`sameType` 就是指针比较）。S03 实测踩过：这里另起一个 TypeContext
//    会让 `getarray(int a[])` 与实参 `int[100]` 判成"类型不符"，
//    490 个用例里立刻有 100 多个误报。
TypeContext& libTypes() { return typeContext(); }

// 中立签名到前端类型的适配；类型仍由同一 TypeContext 驻留。
const Type* frontendType(runtime::Kind kind) {
  TypeContext& t = libTypes();
  switch (kind) {
    case runtime::Kind::Void: return t.voidType();
    case runtime::Kind::Int: return t.intType();
    case runtime::Kind::Float: return t.floatType();
    case runtime::Kind::IntArray: return t.arrayOf(t.intType(), kUnknownDim);
    case runtime::Kind::FloatArray: return t.arrayOf(t.floatType(), kUnknownDim);
  }
  return nullptr;
}

// 用一个小工厂把"表"写成声明式的：每个条目在首次调用时构造一次。
struct Lib {
  std::vector<RuntimeFunc> funcs;
  std::vector<FuncSig> sigs;

  const FuncSig* mk(const Type* ret, std::vector<const Type*> params, bool uncallable = false) {
    sigs.push_back(FuncSig{ret, std::move(params), uncallable});
    return &sigs.back();
  }

  Lib() {
    sigs.reserve(runtime::kSignatures.size());
    for (const runtime::Signature& signature : runtime::kSignatures) {
      std::vector<const Type*> params;
      for (size_t i = 0; i < signature.parameterCount; ++i)
        params.push_back(frontendType(signature.parameters[i]));
      funcs.push_back({signature.name,
                      mk(frontendType(signature.result), std::move(params),
                         signature.uncallable)});
    }
  }
};

const Lib& lib() {
  static const Lib instance;
  return instance;
}

}  // namespace

const std::vector<RuntimeFunc>& runtimeFunctions() { return lib().funcs; }

const FuncSig* lookupRuntimeFunc(const std::string& name) {
  for (const RuntimeFunc& f : lib().funcs) {
    if (name == f.name) return f.sig;
  }
  return nullptr;
}

}  // namespace sysy
