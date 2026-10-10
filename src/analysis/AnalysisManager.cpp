#include "analysis/AnalysisManager.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sysy { namespace flat {
RevisionSnapshot captureRevision(const IRRevision& revision, MutationKind mask) {
  return {mask, revision};
}
bool matchesRevision(const RevisionSnapshot& snapshot, const IRRevision& current) {
  const IRRevision& r = snapshot.revision;
  const MutationKind m = snapshot.mask;
  return (!hasMutation(m, MutationKind::DataFlow) || r.dataFlow == current.dataFlow) &&
         (!hasMutation(m, MutationKind::CFG) || r.cfg == current.cfg) &&
         (!hasMutation(m, MutationKind::Memory) || r.memory == current.memory) &&
         (!hasMutation(m, MutationKind::Calls) || r.calls == current.calls) &&
         (!hasMutation(m, MutationKind::Globals) || r.globals == current.globals) &&
         (!hasMutation(m, MutationKind::Interface) || r.interface == current.interface);
}
namespace {
struct Key {
  std::type_index type;
  const Function* function;
  bool operator==(const Key& o) const {
    return type == o.type && function == o.function;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return k.type.hash_code() ^ (std::hash<const Function*>{}(k.function) << 1);
  }
};
struct Dependency { Key key; uint64_t generation; };
struct Entry {
  RevisionSnapshot snapshot;
  uint64_t generation;
  std::shared_ptr<const void> result;
  std::vector<Dependency> dependencies;
};
struct Frame {
  Key key;
  std::vector<Dependency> dependencies;
  bool failed = false;
};
} // namespace

struct AnalysisManager::State {
  explicit State(Module& m) : module(m) {}
  Module& module;
  std::unordered_map<Key, Entry, KeyHash> cache;
  std::vector<Frame> active;
  const IRRevision& revision(const Key& k) const {
    return k.function ? k.function->revision() : module.revision();
  }
  bool current(const Key& k, uint64_t generation,
               std::unordered_set<Key, KeyHash>& visiting) const {
    auto it = cache.find(k);
    if (it == cache.end() || it->second.generation != generation ||
        !matchesRevision(it->second.snapshot, revision(k)) ||
        !visiting.insert(k).second) return false;
    for (const Dependency& dep : it->second.dependencies)
      if (!current(dep.key, dep.generation, visiting)) return false;
    visiting.erase(k);
    return true;
  }
  bool current(const Key& k, uint64_t generation) const {
    std::unordered_set<Key, KeyHash> visiting;
    return current(k, generation, visiting);
  }
  void record(const Key& k, uint64_t generation) {
    if (!active.empty()) active.back().dependencies.push_back({k, generation});
  }
  void failParent() {
    if (!active.empty()) active.back().failed = true;
  }
};

AnalysisManager::AnalysisManager(Module& module)
  : module_(module), state_(std::make_shared<State>(module)) {}
AnalysisManager::~AnalysisManager() = default;
bool AnalysisManager::current(const std::weak_ptr<State>& weak,
                              std::type_index type, const Function* function,
                              uint64_t generation) {
  auto state = weak.lock();
  return state && state->current(Key{type, function}, generation);
}
AnalysisManager::Response AnalysisManager::fetch(
    std::type_index type, const Function* function, MutationKind mask,
    const std::function<std::shared_ptr<const void>()>& compute) {
  State& state = *state_;
  Key key{type, function};
  if ((function && function->parent() != &module_) ||
      std::any_of(state.active.begin(), state.active.end(),
        [&](const Frame& f) { return f.key == key; })) {
    state.failParent();
    return {};
  }
  auto previous = state.cache.find(key);
  if (previous != state.cache.end() && state.current(key, previous->second.generation)) {
    state.record(key, previous->second.generation);
    return {previous->second.result, previous->second.generation};
  }
  uint64_t generation = previous == state.cache.end() ? 1 : previous->second.generation + 1;
  RevisionSnapshot snapshot = captureRevision(state.revision(key), mask);
  state.active.push_back({key, {}, false});
  std::shared_ptr<const void> result;
  try { result = compute(); }
  catch (...) { /* Fail closed without caching a partial result. */ }
  Frame frame = std::move(state.active.back());
  state.active.pop_back();
  bool ok = result && !frame.failed && matchesRevision(snapshot, state.revision(key));
  if (ok) for (const Dependency& dep : frame.dependencies) {
    if (!state.current(dep.key, dep.generation)) { ok = false; break; }
  }
  if (!ok) {
    state.failParent();
    return {};
  }
  state.cache.insert_or_assign(key, Entry{snapshot, generation, result,
                                          std::move(frame.dependencies)});
  state.record(key, generation);
  return {std::move(result), generation};
}
}} // namespace sysy::flat
