#include "ToyIR.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void report(const std::string& path, const std::string& input, const toy::Module& module,
            const std::string& violation) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write contract report: " + path);
  out << "{\n  \"schema_version\": 1,\n  \"backend\": \"self-built-toy-x86\",\n"
      << "  \"input_sha256\": " << toy::jsonQuote(toy::sha256File(input))
      << ",\n  \"opcode_counts\": {";
  bool first=true;
  for (const auto& p : module.opcodes) {
    if (!first) out << ',';
    out << "\n    " << toy::jsonQuote(p.first) << ": " << p.second;
    first=false;
  }
  if (!first) out << '\n';
  out << "  },\n  \"contract_checks\": {";
  first=true;
  const std::string all[] = {"instruction_set","entry_alloca","phi_placement","phi_edges",
      "gep_i64","inbounds_absence","overflow_flags_absence","zero_global",
      "array_param_ptr","fcmp_predicate","i64_usage","main_i32"};
  for (const auto& name : all) {
    if (!first) out << ',';
    first=false;
    auto it=module.checks.find(name);
    uint64_t observed = it == module.checks.end() ? 0 : it->second;
    bool checked = name != "i64_usage";  // Old four-use restriction is documented stale.
    std::string status = !checked ? "CONTRACT_DOC_STALE" :
                         !violation.empty() ? "VIOLATION" :
                         observed ? "STATIC_OBSERVED" : "UNEXERCISED";
    out << "\n    " << toy::jsonQuote(name)
        << ": {\"checked\": " << (checked?"true":"false")
        << ", \"observed\": " << observed
        << ", \"pass\": " << ((checked && violation.empty())?"true":"false")
        << ", \"status\": " << toy::jsonQuote(status)
        << ", \"dynamically_exercised\": false}";
  }
  out << "\n  },\n  \"violations\": [";
  if (!violation.empty()) out << toy::jsonQuote(violation);
  out << "]\n}\n";
}
} // namespace
int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "self-built-toy-x86 1\n";
    return 0;
  }
  std::string input, output, json;
  for (int i=1;i<argc;++i) {
    const std::string arg=argv[i];
    if ((arg == "-o" || arg == "--report") && i+1<argc) {
      (arg == "-o" ? output : json) = argv[++i];
    } else if (!arg.empty() && arg[0] != '-' && input.empty()) input=arg;
    else {
      std::cerr << "usage: toy_backend input.ll -o output.s [--report output.json]\n";
      return 2;
    }
  }
  if (input.empty() || output.empty()) {
    std::cerr << "usage: toy_backend input.ll -o output.s [--report output.json]\n";
    return 2;
  }
  toy::Module m;
  try {
    m = toy::parseModule(input);
    toy::lowerModule(m,output);
    if (!json.empty()) report(json,input,m,"");
    return 0;
  } catch (const toy::ParseError& error) {
    std::string detail="line "+std::to_string(error.line)+": "+error.what()+
        "\n  source: "+error.source;
    std::cerr << detail << '\n';
    std::remove(output.c_str());
    if (!json.empty()) try { report(json,input,m,detail); } catch (...) {}
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "toy lowering failure: " << error.what() << '\n';
    std::remove(output.c_str());
    if (!json.empty()) try { report(json,input,m,error.what()); } catch (...) {}
    return 1;
  }
}
