#include "ToyIR.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace toy {
namespace {
bool sameType(const Type& a, const Type& b) {
  if (a.name != b.name || a.count != b.count || a.children.size() != b.children.size())
    return false;
  for (size_t i = 0; i < a.children.size(); ++i)
    if (!sameType(a.children[i], b.children[i])) return false;
  return true;
}
void need(bool yes, const std::string& text) {
  if (!yes) throw std::runtime_error(text);
}
uint64_t number(const std::string& s) {
  size_t n = 0;
  uint64_t result = std::stoull(s, &n, 10);
  need(n == s.size() && s[0] != '-', "invalid unsigned integer " + s);
  return result;
}
uint64_t alignment(Cursor& c) {
  if (!c.accept(",")) return 4;
  c.expect("align");
  uint64_t a = number(c.take());
  need(a && !(a & (a - 1)) && a <= 4096, "invalid alignment");
  return a;
}
std::string local(Cursor& c) {
  std::string n = c.take();
  need(n.size() > 1 && n[0] == '%' && n[1] != '"', "expected %local name");
  return n;
}
Value typed(Cursor& c) {
  Type t = parseType(c);
  if (c.accept("align")) {
    (void)number(c.take());
  }
  return parseValue(c, t);
}
void initializer(Cursor& c, const Type& ty, uint64_t offset,
                 std::map<uint64_t, uint32_t>& words, int depth) {
  need(depth < 64, "aggregate nesting exceeds parser limit");
  if (c.accept("zeroinitializer")) return;
  if (ty.name == "i32" || ty.name == "float") {
    std::string literal = c.take();
    need(!literal.empty(), "missing scalar initializer");
    uint32_t bits = 0;
    if (ty.name == "float") bits = floatBits(literal);
    else {
      size_t n = 0;
      long long value = std::stoll(literal, &n, 0);
      need(n == literal.size(), "invalid integer initializer");
      bits = static_cast<uint32_t>(value);
    }
    if (bits) words.emplace(offset, bits);
    return;
  }
  need(ty.isArray(), "unsupported global initializer type " + ty.name);
  c.expect("[");
  for (uint64_t i = 0; i < ty.count; ++i) {
    if (i) c.expect(",");
    Type element = parseType(c);
    need(sameType(element, ty.children[0]), "aggregate element type mismatch");
    initializer(c, element, offset + i * element.size, words, depth + 1);
  }
  c.expect("]");
}
Global global(Cursor& c) {
  Global g;
  g.name = symbolName(c.take());
  c.expect("=");
  g.external = c.accept("external");
  if (g.external) c.expect("global");
  else c.expect("global");
  g.type = parseType(c);
  need(g.type.size && g.type.name != "ptr" && g.type.name != "i64" &&
       g.type.name != "double", "unsupported global storage type");
  if (!g.external) {
    initializer(c, g.type, 0, g.words, 0);
    g.align = alignment(c);
  }
  c.end();
  return g;
}
Function header(Cursor& c, bool declaration) {
  Function fn;
  fn.declaration = declaration;
  fn.result = parseType(c);
  fn.name = symbolName(c.take());
  c.expect("(");
  if (!c.accept(")")) {
    do {
      if (c.accept("...")) { fn.variadic = true; break; }
      Type ty = parseType(c);
      need(ty.name != "void" && !ty.isArray(), "function parameter must be scalar/ptr");
      Value arg{ty, ""};
      if (!declaration) arg.text = local(c);
      fn.params.push_back(std::move(arg));
    } while (c.accept(","));
    c.expect(")");
  }
  if (!declaration) {
    need(!fn.variadic, "variadic definition unsupported");
    c.expect("{");
  }
  c.end();
  return fn;
}
Inst instruction(Cursor& c) {
  Inst i;
  if (!c.peek().empty() && c.peek()[0] == '%') {
    i.dst = local(c);
    c.expect("=");
  }
  i.op = c.take();
  const std::string& op = i.op;
  static const std::set<std::string> binary = {
      "add","sub","mul","sdiv","srem","shl","lshr","ashr","and","or","xor",
      "fadd","fsub","fmul","fdiv"};
  static const std::set<std::string> casts = {
      "sitofp","fptosi","fpext","sext","zext","trunc"};
  if (op == "ret") {
    i.type = parseType(c);
    if (i.type.name != "void") i.values.push_back(parseValue(c, i.type));
  } else if (op == "unreachable") {
  } else if (op == "br") {
    if (c.accept("label")) {
      i.target = local(c);
    } else {
      c.expect("i1");
      i.values.push_back(parseValue(c, {"i1",1,0,{}}));
      c.expect(","); c.expect("label"); i.target = local(c);
      c.expect(","); c.expect("label"); i.alternate = local(c);
    }
  } else if (op == "alloca") {
    i.sourceType = parseType(c);
    need(i.sourceType.size && i.sourceType.name != "void", "invalid alloca type");
    (void)alignment(c);
    i.type = {"ptr",8,0,{}};
  } else if (op == "load") {
    i.type = parseType(c);
    c.expect(","); c.expect("ptr");
    i.values.push_back(parseValue(c, {"ptr",8,0,{}}));
    (void)alignment(c);
  } else if (op == "store") {
    i.values.push_back(typed(c));
    c.expect(","); c.expect("ptr");
    i.values.push_back(parseValue(c, {"ptr",8,0,{}}));
    (void)alignment(c);
  } else if (op == "getelementptr") {
    i.sourceType = parseType(c);
    need(i.sourceType.size > 0, "invalid GEP source element type");
    c.expect(","); c.expect("ptr");
    i.values.push_back(parseValue(c, {"ptr",8,0,{}}));
    c.expect(","); c.expect("i64");
    i.values.push_back(parseValue(c, {"i64",8,0,{}}));
    need(!c.accept(","), "GEP subset permits exactly one i64 index");
    i.type = {"ptr",8,0,{}};
  } else if (op == "icmp" || op == "fcmp") {
    i.pred = c.take();
    static const std::set<std::string> ip = {"eq","ne","slt","sle","sgt","sge"};
    static const std::set<std::string> fp = {"oeq","une","olt","ole","ogt","oge"};
    need((op == "icmp" ? ip : fp).count(i.pred) != 0, "unsupported comparison predicate " + i.pred);
    Type operandType = parseType(c);
    need(op == "fcmp" ? operandType.name == "float" :
        (operandType.name == "i32" || operandType.name == "i64"),
        "invalid comparison operand type");
    i.values.push_back(parseValue(c, operandType));
    c.expect(",");
    i.values.push_back(parseValue(c, operandType));
    i.type = {"i1",1,0,{}};
  } else if (binary.count(op)) {
    i.type = parseType(c);
    need(op[0] == 'f' ? i.type.name == "float" :
         (i.type.name == "i32" || i.type.name == "i64"),
         "invalid binary arithmetic type");
    i.values.push_back(parseValue(c, i.type));
    c.expect(",");
    i.values.push_back(parseValue(c, i.type));
  } else if (op == "fneg") {
    i.type = parseType(c);
    need(i.type.name == "float", "fneg requires float");
    i.values.push_back(parseValue(c, i.type));
  } else if (casts.count(op)) {
    Type from = parseType(c);
    i.values.push_back(parseValue(c, from));
    c.expect("to");
    i.type = parseType(c);
    bool valid =
      (op == "sitofp" && (from.name == "i32" || from.name == "i64") && i.type.name == "float") ||
      (op == "fptosi" && from.name == "float" && (i.type.name == "i32" || i.type.name == "i64")) ||
      (op == "fpext" && from.name == "float" && i.type.name == "double") ||
      (op == "sext" && from.name == "i32" && i.type.name == "i64") ||
      (op == "zext" && ((from.name == "i1" && i.type.name == "i32") ||
                       (from.name == "i32" && i.type.name == "i64"))) ||
      (op == "trunc" && ((from.name == "i64" && i.type.name == "i32") ||
                        (from.name == "i32" && i.type.name == "i8")));
    need(valid, "unsupported cast type combination");
  } else if (op == "select") {
    c.expect("i1");
    i.values.push_back(parseValue(c, {"i1",1,0,{}}));
    c.expect(",");
    Value yes = typed(c);
    c.expect(",");
    Value no = typed(c);
    need(sameType(yes.type, no.type), "select arm types differ");
    i.type = yes.type;
    i.values.push_back(std::move(yes));
    i.values.push_back(std::move(no));
  } else if (op == "phi") {
    i.type = parseType(c);
    need(!i.type.isArray() && i.type.name != "void", "unsupported phi type");
    do {
      c.expect("[");
      std::string src = parseValue(c, i.type).text;
      c.expect(",");
      i.incoming.push_back({local(c), src});
      c.expect("]");
    } while (c.accept(","));
    need(!i.incoming.empty(), "phi requires predecessor entries");
  } else if (op == "call") {
    i.type = parseType(c);
    i.callee = symbolName(c.take());
    c.expect("(");
    if (!c.accept(")")) {
      do { i.values.push_back(typed(c)); } while (c.accept(","));
      c.expect(")");
    }
    if (i.callee == "llvm.memcpy.p0.p0.i64" || i.callee == "llvm.memset.p0.i64") {
      need(i.type.name == "void" && i.values.size() == 4, "intrinsic signature mismatch");
      const bool cp = i.callee == "llvm.memcpy.p0.p0.i64";
      need(i.values[0].type.name == "ptr" &&
           i.values[1].type.name == (cp ? "ptr" : "i8") &&
           i.values[2].type.name == "i64" && i.values[3].type.name == "i1" &&
           i.values[3].text == "false", "invalid memcpy/memset argument contract");
    }
  } else {
    throw std::runtime_error("unsupported/unknown LLVM opcode '" + op + "'");
  }
  c.end();
  const bool hasResult = !i.dst.empty();
  const bool needsResult = op == "alloca" || op == "load" || op == "getelementptr" ||
      op == "icmp" || op == "fcmp" || op == "phi" || op == "select" ||
      op == "fneg" || binary.count(op) || casts.count(op) ||
      (op == "call" && i.type.name != "void");
  need(hasResult == needsResult, "result assignment presence/type mismatch for " + op);
  return i;
}
void validateFunction(const Function& f, Module& module) {
  if (f.declaration) return;
  need(!f.blocks.empty(), "function missing basic blocks");
  std::set<std::string> labels;
  std::unordered_map<std::string, std::set<std::string>> predecessors;
  std::unordered_map<std::string, Type> definitions;
  for (const Value& a : f.params)
    need(definitions.emplace(a.text, a.type).second, "duplicate parameter");
  for (const Block& b : f.blocks)
    need(labels.insert("%" + b.label).second, "duplicate basic block " + b.label);
  for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
    const Block& b = f.blocks[bi];
    need(!b.instructions.empty(), "empty basic block " + b.label);
    bool seenNormal = false;
    for (size_t ii = 0; ii < b.instructions.size(); ++ii) {
      const Inst& inst = b.instructions[ii];
      try {
      bool terminator = inst.op == "ret" || inst.op == "br" || inst.op == "unreachable";
      need(terminator == (ii + 1 == b.instructions.size()), "missing/early terminator");
      if (inst.op == "ret")
        need(sameType(inst.type, f.result), "function return type mismatch");
      need(inst.op != "phi" || !seenNormal, "phi must be contiguous at block entry");
      if (inst.op != "phi") seenNormal = true;
      need(inst.op != "alloca" || bi == 0, "alloca must be in entry block");
      if (!inst.dst.empty())
        need(definitions.emplace(inst.dst, inst.type).second,
             "duplicate local definition " + inst.dst);
      if (inst.op == "br") {
        predecessors[inst.target].insert("%" + b.label);
        if (!inst.alternate.empty()) predecessors[inst.alternate].insert("%" + b.label);
      }
      } catch (const std::exception& e) {
        throw ParseError(inst.line, inst.source, e.what());
      }
    }
  }
  for (const auto& pair : predecessors)
    need(labels.count(pair.first), "branch references unknown block " + pair.first);
  for (const Block& b : f.blocks) for (const Inst& inst : b.instructions) {
    try {
    if (inst.op == "br") {
      need(labels.count(inst.target), "unknown branch destination " + inst.target);
      if (!inst.alternate.empty())
        need(labels.count(inst.alternate), "unknown branch destination " + inst.alternate);
    }
    for (const Value& v : inst.values) {
      if (!v.text.empty() && v.text[0] == '%') {
        auto it = definitions.find(v.text);
        need(it != definitions.end(), "unknown local use " + v.text);
        need(sameType(it->second, v.type), "SSA local use type mismatch " + v.text);
      }
    }
    if (inst.op == "phi") {
      std::set<std::string> seen;
      for (const PhiEdge& edge : inst.incoming) {
        need(labels.count(edge.predecessor) && seen.insert(edge.predecessor).second,
             "bad phi predecessor/duplicate edge " + edge.predecessor);
        need(predecessors["%" + b.label].count(edge.predecessor),
             "phi input does not match CFG predecessor");
        if (!edge.source.empty() && edge.source[0] == '%') {
          auto it = definitions.find(edge.source);
          need(it != definitions.end() && sameType(it->second, inst.type),
               "unknown/mistyped phi local " + edge.source);
        }
      }
      need(seen == predecessors["%" + b.label], "phi predecessor completeness mismatch");
      ++module.checks["phi_edges"];
    }
    if (inst.op == "getelementptr") ++module.checks["gep_i64"];
    if (inst.op == "alloca") ++module.checks["entry_alloca"];
    if (inst.op == "fcmp") ++module.checks["fcmp_predicate"];
    } catch (const std::exception& e) {
      throw ParseError(inst.line, inst.source, e.what());
    }
  }
}
} // namespace

Module parseModule(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open input LLVM IR: " + path);
  Module module;
  std::string line;
  Function* current = nullptr;
  int lineNo = 0;
  std::set<std::string> names;
  while (std::getline(in, line)) {
    ++lineNo;
    Cursor c(line);
    try {
      std::string first = c.peek();
      if (first.empty()) continue;
      if (current) {
        if (c.accept("}")) {
          c.end();
          validateFunction(*current, module);
          current = nullptr;
          continue;
        }
        if (first.size() >= 3 && first.substr(0,2) == "bb" &&
            first.back() == ':') {
          std::string label = c.take();
          c.end();
          current->blocks.push_back({label.substr(0,label.size()-1),{}});
        } else {
          need(!current->blocks.empty(), "instruction before first basic block");
          Inst inst = instruction(c);
          inst.line = lineNo;
          inst.source = line;
          ++module.opcodes[inst.op];
          ++module.checks["instruction_set"];
          if (inst.op == "phi") ++module.checks["phi_placement"];
          if (inst.op == "getelementptr") ++module.checks["inbounds_absence"];
          if (inst.op == "add" || inst.op == "sub" || inst.op == "mul" ||
              inst.op == "fadd" || inst.op == "fsub" || inst.op == "fmul")
            ++module.checks["overflow_flags_absence"];
          current->blocks.back().instructions.push_back(std::move(inst));
        }
      } else if (first[0] == '@') {
        Global g = global(c);
        need(names.insert(g.name).second, "duplicate global symbol");
        if (!g.external && g.words.empty()) ++module.checks["zero_global"];
        module.globals.push_back(std::move(g));
      } else if (c.accept("declare")) {
        Function f = header(c, true);
        need(names.insert(f.name).second, "duplicate declaration/symbol");
        for (const Value& p : f.params)
          if (p.type.name == "ptr") ++module.checks["array_param_ptr"];
        module.functions.push_back(std::move(f));
      } else if (c.accept("define")) {
        Function f = header(c, false);
        need(names.insert(f.name).second, "duplicate definition/symbol");
        if (f.name == "main") {
          need(f.result.name == "i32", "main must return i32");
          ++module.checks["main_i32"];
        }
        for (const Value& p : f.params)
          if (p.type.name == "ptr") ++module.checks["array_param_ptr"];
        module.functions.push_back(std::move(f));
        current = &module.functions.back();
      } else {
        throw std::runtime_error("unknown top-level directive / target-specific metadata: " + first);
      }
    } catch (const ParseError&) { throw; }
      catch (const std::exception& e) { throw ParseError(lineNo, line, e.what()); }
  }
  if (current) throw ParseError(lineNo, line, "unterminated function; expected '}'");
  std::unordered_map<std::string, const Function*> signatures;
  for (const Function& f : module.functions) signatures.emplace(f.name, &f);
  for (const Function& f : module.functions) for (const Block& b : f.blocks)
    for (const Inst& i : b.instructions) {
      if (i.op != "call") continue;
      auto found = signatures.find(i.callee);
      if (found == signatures.end())
        throw ParseError(i.line, i.source, "missing callee declaration/definition");
      const Function& callee = *found->second;
      bool valid = sameType(i.type,callee.result) &&
                   (callee.variadic ? i.values.size() >= callee.params.size() :
                                     i.values.size() == callee.params.size());
      for (size_t n = 0; valid && n < callee.params.size(); ++n)
        valid = sameType(i.values[n].type,callee.params[n].type);
      for (size_t n=callee.params.size(); valid && n<i.values.size(); ++n)
        valid = i.values[n].type.name == "i32" ||
                i.values[n].type.name == "i64" ||
                i.values[n].type.name == "double";
      if (!valid) throw ParseError(i.line, i.source,
                                    "call signature does not match declaration/definition");
    }
  return module;
}
} // namespace toy
