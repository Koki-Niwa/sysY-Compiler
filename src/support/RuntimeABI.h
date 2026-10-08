// RuntimeABI: source-visible runtime builtins to target ABI entry points.
//
// This catalog is deliberately separate from RuntimeLib.h.  RuntimeLib owns
// the names that SysY source may use; this file owns only the lowering
// boundary to implementation symbols.  ABI-only names therefore never become
// valid source identifiers through this interface.
#ifndef SYSY_SUPPORT_RUNTIMEABI_H
#define SYSY_SUPPORT_RUNTIMEABI_H

#include <array>
#include <string_view>

namespace sysy {
namespace runtime {

// A source builtin may lower to a different runtime symbol and may require an
// implicit source location argument at that boundary.  The table is generic:
// callers do not need to know which builtin names have a special ABI spelling.
struct AbiMapping {
  std::string_view sourceName;
  std::string_view targetName;
  bool appendCallLine;
};

inline constexpr std::array<AbiMapping, 2> kAbiMappings{{
    {"starttime", "_sysy_starttime", true},
    {"stoptime", "_sysy_stoptime", true},
}};

// Pre: none.
// Post: returns the target ABI mapping for a source-visible builtin, or
//       nullptr when that builtin has no special target lowering.
// Effects: none; the returned entry points into static storage.
inline const AbiMapping* lookupAbi(std::string_view sourceName) {
  for (const AbiMapping& mapping : kAbiMappings) {
    if (sourceName == mapping.sourceName) return &mapping;
  }
  return nullptr;
}

}  // namespace runtime
}  // namespace sysy

#endif  // SYSY_SUPPORT_RUNTIMEABI_H
