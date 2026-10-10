// Flat IR cache keyed by analysis type, scope and revision dependencies.
#ifndef SYSY_ANALYSIS_ANALYSISMANAGER_H
#define SYSY_ANALYSIS_ANALYSISMANAGER_H
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <utility>
#include "ir/Module.h"
namespace sysy { namespace flat {
enum class AnalysisScope { Module, Function };
struct RevisionSnapshot {
  MutationKind mask = MutationKind::None;
  IRRevision revision;
};
RevisionSnapshot captureRevision(const IRRevision&, MutationKind);
bool matchesRevision(const RevisionSnapshot&, const IRRevision&);

template <typename T> class AnalysisHandle {
 public:
  AnalysisHandle() = default;
  bool isCurrent() const {
    return storage_ && !lifetime_.expired() && valid_ && valid_(generation_);
  }
  const T* get() const { return isCurrent() ? storage_.get() : nullptr; }
  uint64_t generation() const { return generation_; }
 private:
  friend class AnalysisManager;
  AnalysisHandle(std::weak_ptr<const void> lifetime, std::shared_ptr<const T> storage,
                 std::function<bool(uint64_t)> valid, uint64_t generation)
      : lifetime_(std::move(lifetime)), storage_(std::move(storage)),
        valid_(std::move(valid)), generation_(generation) {}
  std::weak_ptr<const void> lifetime_;
  std::shared_ptr<const T> storage_;
  std::function<bool(uint64_t)> valid_;
  uint64_t generation_ = 0;
};

class AnalysisManager {
 public:
  explicit AnalysisManager(Module&);
  ~AnalysisManager();
  AnalysisManager(const AnalysisManager&) = delete;
  AnalysisManager& operator=(const AnalysisManager&) = delete;
  Module& module() const { return module_; }

  template <typename A> AnalysisHandle<typename A::Result> get() {
    static_assert(A::scope == AnalysisScope::Module, "Module scope required");
    using R = typename A::Result;
    return request<R>(typeid(A), nullptr, A::revisionDependencies(),
      [&]() -> std::shared_ptr<const void> {
        std::optional<R> result = A::run(static_cast<const Module&>(module_), *this);
        return result ? std::make_shared<const R>(std::move(*result)) : nullptr;
      });
  }
  template <typename A> AnalysisHandle<typename A::Result> get(const Function& f) {
    static_assert(A::scope == AnalysisScope::Function, "Function scope required");
    using R = typename A::Result;
    return request<R>(typeid(A), &f, A::revisionDependencies(),
      [&]() -> std::shared_ptr<const void> {
        std::optional<R> result = A::run(f, *this);
        return result ? std::make_shared<const R>(std::move(*result)) : nullptr;
      });
  }
 private:
  struct State;
  struct Response {
    std::shared_ptr<const void> storage;
    uint64_t generation = 0;
  };
  Response fetch(std::type_index, const Function*, MutationKind,
                 const std::function<std::shared_ptr<const void>()>&);
  static bool current(const std::weak_ptr<State>&, std::type_index,
                      const Function*, uint64_t);
  template <typename R> AnalysisHandle<R> request(
      std::type_index type, const Function* function, MutationKind mask,
      const std::function<std::shared_ptr<const void>()>& compute) {
    Response r = fetch(type, function, mask, compute);
    if (!r.storage) return {};
    std::weak_ptr<State> weak = state_;
    return AnalysisHandle<R>(state_, std::static_pointer_cast<const R>(r.storage),
      [weak, type, function](uint64_t generation) {
        return current(weak, type, function, generation);
      }, r.generation);
  }
  Module& module_;
  std::shared_ptr<State> state_;
};
}} // namespace sysy::flat
#endif
