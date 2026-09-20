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

namespace sysy {
namespace {

// ⚠️ **必须**用全局唯一的那个类型工厂（Type.h 的 typeContext()）：
//    用户代码里的类型与运行时库形参的类型要能**指针相等**地比较
//    （`sameType` 就是指针比较）。S03 实测踩过：这里另起一个 TypeContext
//    会让 `getarray(int a[])` 与实参 `int[100]` 判成"类型不符"，
//    490 个用例里立刻有 100 多个误报。
TypeContext& libTypes() { return typeContext(); }

// 数组形参的类型：`int a[]` → Array(Int, kUnknownDim)；`float a[]` 同理。
const Type* paramArray(const Type* elem) {
  return libTypes().arrayOf(elem, kUnknownDim);
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
    TypeContext& t = libTypes();
    const Type* i = t.intType();
    const Type* f = t.floatType();
    const Type* v = t.voidType();
    const Type* ia = paramArray(i);
    const Type* fa = paramArray(f);

    sigs.reserve(13);
    funcs.push_back({"getint",     mk(i, {})});
    funcs.push_back({"getch",      mk(i, {})});
    funcs.push_back({"getarray",   mk(i, {ia})});
    funcs.push_back({"getfloat",   mk(f, {})});
    funcs.push_back({"getfarray",  mk(i, {fa})});
    funcs.push_back({"putint",     mk(v, {i})});
    funcs.push_back({"putch",      mk(v, {i})});
    funcs.push_back({"putarray",   mk(v, {i, ia})});
    funcs.push_back({"putfloat",   mk(v, {f})});
    funcs.push_back({"putfarray",  mk(v, {i, fa})});
    funcs.push_back({"putf",       mk(v, {}, /*uncallable=*/true)});
    funcs.push_back({"starttime",  mk(v, {})});
    funcs.push_back({"stoptime",   mk(v, {})});
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
