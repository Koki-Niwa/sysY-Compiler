// ============================================================================
// RuntimeLib —— SysY 运行时库的函数签名表（S03 交付物 #3）
//
// 规范依据：`docs/sysy_lang.txt` §1 Overview（"SysY itself does not provide
// built-in language constructs for I/O. I/O is provided by way of a runtime
// library"）与官方 `runtime/sylib.h`。
//
// ── 合规说明（**这是全项目唯一允许的"名字表"**）──────────────────────────
//   COMPLIANCE.md §二明确允许"使用官方运行时库"，而铁律 2 禁的是
//   "识别函数名做**特定优化**"。本表**只用于类型检查与（S05 的）ABI 发射**，
//   不改变任何优化行为：`verify.sh` 的 B 组 grep 明确豁免文件名含
//   `RuntimeLib` 的文件（见 COMPLIANCE.md §三）。
//
// ── `starttime()` / `stoptime()` 是**宏**，不是函数 ──────────────────────
//   `sylib.h` 里：
//       #define starttime() _sysy_starttime(__LINE__)
//       #define stoptime()  _sysy_stoptime(__LINE__)
//   SysY 没有宏，所以编译器必须把 `starttime()` 认成**零参内建函数**，
//   并在降级时把**调用点的行号**作为实参传给 `_sysy_starttime` ——
//   这正是 D11 要求 `SourceLoc` 一路带到 IR 的原因。
//   ★ S03 只负责在名字表里提供这两个零参函数；**行号的插入是 S05 的事**。
//
// ── `putf` 是变参且首参是 `char*`，SysY 里没有字符串类型 ─────────────────
//   让它以"不可调用"（`uncallable`）的形式存在于表里即可：调用一律报
//   `E-CALL-VARARGS`。（规范 §1 说"编译器必须能处理这类情形并把参数正确
//   传给运行时库"——但语料实测 540 个文件里 `putf` 出现 **0 次**，
//   而 SysY 侧根本没有能构造 `char*` 的表达式，所以"正确处理"就是"报错"。）
//
// ── `_sysy_starttime` / `_sysy_stoptime` **不在**本表里 ──────────────────
//   源码里写的是 `starttime()` / `stoptime()`；下划线开头的两个名字是
//   运行时库的实现符号，**不是 SysY 层的可见名字**。把它们放进用户可见的
//   名字表会让非法程序（调用 `_sysy_starttime(1)`）被判成合法。
// ============================================================================
#ifndef SYSY_FRONTEND_RUNTIMELIB_H
#define SYSY_FRONTEND_RUNTIMELIB_H

#include <string>
#include <vector>

#include "frontend/Type.h"

namespace sysy {

// 一个运行时库函数：SysY 侧可见的名字 + 签名。
struct RuntimeFunc {
  const char* name = nullptr;
  const FuncSig* sig = nullptr;
};

// 【后置】返回全部 13 个运行时函数的表（顺序 = `--emit=sema` 的打印顺序，
//         也是 prompt §3.4 的表顺序）。表是静态的，生命期 = 进程。
const std::vector<RuntimeFunc>& runtimeFunctions();

// 【后置】按名字查签名；不是运行时函数 → nullptr。
//   ⚠️ 只做一次线性查找（13 项）。**不要**在这里加任何"名字特判"逻辑：
//      返回值只允许用于类型检查与 ABI 发射（铁律 2）。
const FuncSig* lookupRuntimeFunc(const std::string& name);

// 【后置】该名字是不是运行时库函数名（用于"用户函数不得与运行时库重名"）。
inline bool isRuntimeFuncName(const std::string& name) {
  return lookupRuntimeFunc(name) != nullptr;
}

}  // namespace sysy
#endif  // SYSY_FRONTEND_RUNTIMELIB_H
