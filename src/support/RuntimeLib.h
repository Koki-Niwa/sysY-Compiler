// RuntimeLib: SysY 可见运行时签名的唯一描述表。
// 前端将这些描述适配为自身类型；IR 读回/校验只查询结果形状。
// 不依赖 AST 或任一 IR。签名不构成纯函数/无副作用证明。
#ifndef SYSY_SUPPORT_RUNTIMELIB_H
#define SYSY_SUPPORT_RUNTIMELIB_H

#include <array>
#include <cstddef>
#include <string_view>

namespace sysy {
namespace runtime {

enum class Kind { Void, Int, Float, IntArray, FloatArray };

struct Signature {
  const char* name;
  Kind result;
  std::array<Kind, 2> parameters;
  size_t parameterCount;
  bool uncallable = false;
};

// 顺序属于 sema dump 契约。putf 的 char*/变参无法由当前 SysY 表达，
// 保留既有不可调用表示；此表不是完整的后端 ABI 声明表。
inline constexpr std::array<Signature, 13> kSignatures{{
    {"getint", Kind::Int, {}, 0},
    {"getch", Kind::Int, {}, 0},
    {"getarray", Kind::Int, {Kind::IntArray}, 1},
    {"getfloat", Kind::Float, {}, 0},
    {"getfarray", Kind::Int, {Kind::FloatArray}, 1},
    {"putint", Kind::Void, {Kind::Int}, 1},
    {"putch", Kind::Void, {Kind::Int}, 1},
    {"putarray", Kind::Void, {Kind::Int, Kind::IntArray}, 2},
    {"putfloat", Kind::Void, {Kind::Float}, 1},
    {"putfarray", Kind::Void, {Kind::Int, Kind::FloatArray}, 2},
    {"putf", Kind::Void, {}, 0, true},
    {"starttime", Kind::Void, {}, 0},
    {"stoptime", Kind::Void, {}, 0},
}};

// 前置：无。后置：返回静态描述；未知或仅 ABI 可见的符号返回 nullptr。
// 副作用：无。调用方负责其所在层的类型适配和合法性检查。
inline const Signature* lookup(std::string_view name) {
  for (const Signature& signature : kSignatures) {
    if (name == signature.name) return &signature;
  }
  return nullptr;
}

}  // namespace runtime
}  // namespace sysy
#endif
