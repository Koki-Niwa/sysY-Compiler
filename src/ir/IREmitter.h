// ir/IREmitter.h - common contract for textual IR emitters.
#ifndef SYSY_IR_IREMITTER_H
#define SYSY_IR_IREMITTER_H

#include <string>

namespace sysy {
namespace flat {

class Module;

class IREmitter {
 public:
  virtual ~IREmitter() = default;

  // Emits atomically: output is changed only when this returns true.  On
  // failure, error contains a diagnostic suitable for the caller to report.
  virtual bool emit(const Module& module, std::string& output,
                    std::string& error) const = 0;
};

}  // namespace flat
}  // namespace sysy

#endif  // SYSY_IR_IREMITTER_H
