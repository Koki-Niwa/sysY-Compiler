// RuntimeLib: SysY 可见运行时签名和参考执行行为的唯一描述表。
// 前端将这些描述适配为自身类型；IR 读回/校验只查询结果形状。
// 不依赖 AST 或任一 IR。行为标签不构成纯函数/无副作用证明。
#ifndef SYSY_SUPPORT_RUNTIMELIB_H
#define SYSY_SUPPORT_RUNTIMELIB_H

#include <array>
#include <cstddef>
#include <string_view>

namespace sysy {
namespace runtime {

enum class Kind { Void, Int, Float, IntArray, FloatArray };

// Reference execution behavior belongs beside each source-visible signature,
// so the interpreter can dispatch through the catalog without a second name table.
enum class Behavior {
  Unsupported,
  ReadInt,
  ReadChar,
  ReadFloat,
  ReadIntArray,
  ReadFloatArray,
  WriteInt,
  WriteChar,
  WriteFloat,
  WriteIntArray,
  WriteFloatArray,
  NoOp
};

struct Signature {
  const char* name;
  Kind result;
  std::array<Kind, 2> parameters;
  size_t parameterCount;
  bool uncallable = false;
  Behavior behavior = Behavior::Unsupported;
};

// 顺序属于 sema dump 契约。putf 的 char*/变参无法由当前 SysY 表达，
// 保留既有不可调用表示；此表不是完整的后端 ABI 声明表。
inline constexpr std::array<Signature, 13> kSignatures{{
    {"getint", Kind::Int, {}, 0, false, Behavior::ReadInt},
    {"getch", Kind::Int, {}, 0, false, Behavior::ReadChar},
    {"getarray", Kind::Int, {Kind::IntArray}, 1, false, Behavior::ReadIntArray},
    {"getfloat", Kind::Float, {}, 0, false, Behavior::ReadFloat},
    {"getfarray", Kind::Int, {Kind::FloatArray}, 1, false, Behavior::ReadFloatArray},
    {"putint", Kind::Void, {Kind::Int}, 1, false, Behavior::WriteInt},
    {"putch", Kind::Void, {Kind::Int}, 1, false, Behavior::WriteChar},
    {"putarray", Kind::Void, {Kind::Int, Kind::IntArray}, 2, false,
     Behavior::WriteIntArray},
    {"putfloat", Kind::Void, {Kind::Float}, 1, false, Behavior::WriteFloat},
    {"putfarray", Kind::Void, {Kind::Int, Kind::FloatArray}, 2, false,
     Behavior::WriteFloatArray},
    {"putf", Kind::Void, {}, 0, true, Behavior::Unsupported},
    {"starttime", Kind::Void, {}, 0, false, Behavior::NoOp},
    {"stoptime", Kind::Void, {}, 0, false, Behavior::NoOp},
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
