// ============================================================================
// SourceLoc —— 源码位置（D11：位置信息要一路带进 IR）
//
// 约定（P00 prompt §3.1）：
//   * line / col 都是 1-based
//   * 0 表示"无位置信息"（line == 0 时 col 无意义，渲染时省略列号）
//   * 只存两个 uint32_t —— 可以自由按值传递、塞进指令、比较相等
// ============================================================================
#ifndef SYSY_SUPPORT_SOURCELOC_H
#define SYSY_SUPPORT_SOURCELOC_H

#include <cstdint>

namespace sysy {

struct SourceLoc {
  uint32_t line = 0;   // 1-based；0 = 无位置信息
  uint32_t col  = 0;   // 1-based

  constexpr SourceLoc() = default;
  constexpr SourceLoc(uint32_t l, uint32_t c) : line(l), col(c) {}

  // 是否携带可用位置（用于诊断渲染与 IR 行号发射）
  constexpr bool valid() const { return line != 0; }
};

constexpr bool operator==(SourceLoc a, SourceLoc b) {
  return a.line == b.line && a.col == b.col;
}
constexpr bool operator!=(SourceLoc a, SourceLoc b) { return !(a == b); }

}  // namespace sysy

#endif  // SYSY_SUPPORT_SOURCELOC_H
