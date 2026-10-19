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
struct CallGraphInfo;
// CGSCC is keyed by the SCC index inside one CallGraphInfo result. Revisions
// (Calls/Interface) decide freshness; the index is only the cache slot.
enum class AnalysisScope { Module, Function, CGSCC, Loop };
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
  // Cumulative read-only diagnostic: each cache entry expanded during a
  // top-level freshness check contributes once; memo hits do not.  It never
  // affects cache decisions and is intended for bounded traversal tests.
  uint64_t freshnessExpansionCount() const;

  template <typename A> AnalysisHandle<typename A::Result> get() {
    static_assert(A::scope == AnalysisScope::Module, "Module scope required");
    using R = typename A::Result;
    return request<R>(typeid(A), nullptr, 0, nullptr, A::revisionDependencies(),
      [&]() -> std::shared_ptr<const void> {
        std::optional<R> result = A::run(static_cast<const Module&>(module_), *this);
        return result ? std::make_shared<const R>(std::move(*result)) : nullptr;
      });
  }
  template <typename A> AnalysisHandle<typename A::Result> get(const Function& f) {
    static_assert(A::scope == AnalysisScope::Function, "Function scope required");
    using R = typename A::Result;
    return request<R>(typeid(A), &f, 0, nullptr, A::revisionDependencies(),
      [&]() -> std::shared_ptr<const void> {
        std::optional<R> result = A::run(f, *this);
        return result ? std::make_shared<const R>(std::move(*result)) : nullptr;
      });
  }
  // First real Loop scope: the loop index comes from LoopInfoAnalysis for the
  // same Function; revisions decide freshness.
  template <typename A> AnalysisHandle<typename A::Result> get(
      const Function& function, size_t loopIndex) {
    static_assert(A::scope == AnalysisScope::Loop, "Loop scope required");
    using R = typename A::Result;
    return request<R>(typeid(A), &function, static_cast<uint32_t>(loopIndex + 1),
      nullptr, A::revisionDependencies(),
      [&]() -> std::shared_ptr<const void> {
        std::optional<R> result = A::run(function, loopIndex, *this);
        return result ? std::make_shared<const R>(std::move(*result)) : nullptr;
      });
  }
  // First real CGSCC adaptor: the call graph owns the SCC numbering, so the
  // cache key includes the graph object identity. A stale graph therefore
  // cannot read a fresh SCC slot; its own entry fails the revision check.
  template <typename A> AnalysisHandle<typename A::Result> get(
      const CallGraphInfo& callGraph, size_t sccIndex) {
    static_assert(A::scope == AnalysisScope::CGSCC, "CGSCC scope required");
    using R = typename A::Result;
    return request<R>(typeid(A), nullptr, static_cast<uint32_t>(sccIndex + 1),
      &callGraph, A::revisionDependencies(),
      [&]() -> std::shared_ptr<const void> {
        std::optional<R> result = A::run(callGraph, sccIndex, *this);
        return result ? std::make_shared<const R>(std::move(*result)) : nullptr;
      });
  }
 private:
  struct State;
  struct Response {
    std::shared_ptr<const void> storage;
    uint64_t generation = 0;
  };
  Response fetch(std::type_index, const Function*, uint32_t tag,
                 const void* graphKey, MutationKind,
                 const std::function<std::shared_ptr<const void>()>&);
  static bool current(const std::weak_ptr<State>&, std::type_index,
                      const Function*, uint32_t tag, const void* graphKey,
                      uint64_t);
  template <typename R> AnalysisHandle<R> request(
      std::type_index type, const Function* function, uint32_t tag,
      const void* graphKey, MutationKind mask,
      const std::function<std::shared_ptr<const void>()>& compute) {
    Response r = fetch(type, function, tag, graphKey, mask, compute);
    if (!r.storage) return {};
    std::weak_ptr<State> weak = state_;
    return AnalysisHandle<R>(state_, std::static_pointer_cast<const R>(r.storage),
      [weak, type, function, tag, graphKey](uint64_t generation) {
        return current(weak, type, function, tag, graphKey, generation);
      }, r.generation);
  }
  Module& module_;
  std::shared_ptr<State> state_;
};
}} // namespace sysy::flat
#endif
