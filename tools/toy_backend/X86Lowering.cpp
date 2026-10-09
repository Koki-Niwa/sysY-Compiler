#include "X86Lowering.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace toy {
namespace {
uint64_t alignUp(uint64_t n, uint64_t a) {
  if (n > std::numeric_limits<uint64_t>::max() - (a - 1))
    throw std::runtime_error("stack allocation overflow");
  return ((n + a - 1) / a) * a;
}
const char* gpArgs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
const char* fpArgs[] = {"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7"};
} // namespace
void X86Lowering::emit(const std::string& s) { out_ << "  " << s << '\n'; }
std::string X86Lowering::loc(uint64_t n) const { return "-" + std::to_string(n) + "(%rbp)"; }
std::string X86Lowering::slot(const std::string& name) const {
  auto it = locals_.find(name);
  if (it == locals_.end()) throw std::runtime_error("no stack slot for " + name);
  return loc(it->second.offset);
}
uint64_t X86Lowering::reserve(uint64_t size, uint64_t align) {
  cursor_ = alignUp(cursor_, align);
  cursor_ = alignUp(cursor_ + size, align);
  if (cursor_ > static_cast<uint64_t>(INT64_MAX))
    throw std::runtime_error("stack frame too large");
  return cursor_;
}
std::string X86Lowering::reg32(const std::string& s) {
  static const std::map<std::string,std::string> r = {
      {"rax","eax"},{"rcx","ecx"},{"rdx","edx"},{"rdi","edi"},{"rsi","esi"},
      {"r8","r8d"},{"r9","r9d"},{"r10","r10d"},{"r11","r11d"}};
  auto it = r.find(s);
  if (it == r.end()) throw std::runtime_error("invalid scratch register " + s);
  return it->second;
}
bool X86Lowering::wide(const Type& t) {
  return t.name == "ptr" || t.name == "i64" || t.name == "double";
}
std::string X86Lowering::blockLabel(const std::string& b) const {
  std::string name = b[0] == '%' ? b.substr(1) : b;
  return ".Ltoy" + std::to_string(functionIndex_) + "_" + name;
}
std::string X86Lowering::uniqueLabel() {
  return ".Ltoy" + std::to_string(functionIndex_) + "_aux" + std::to_string(labelIndex_++);
}
void X86Lowering::global(const Global& g) {
  if (g.external) return;
  std::string name = gasSymbol(g.name);
  out_ << (g.words.empty() ? ".bss\n" : ".data\n")
       << ".globl " << name << "\n.balign " << g.align << "\n.type " << name
       << ", @object\n.size " << name << ", " << g.type.size << "\n" << name << ":\n";
  if (g.words.empty()) {
    emit(".zero " + std::to_string(g.type.size));
    return;
  }
  uint64_t offset = 0;
  for (const auto& word : g.words) {
    if (word.first < offset || word.first + 4 > g.type.size)
      throw std::runtime_error("global initializer word outside object");
    if (word.first != offset) emit(".zero " + std::to_string(word.first - offset));
    emit(".long " + std::to_string(word.second));
    offset = word.first + 4;
  }
  if (offset < g.type.size) emit(".zero " + std::to_string(g.type.size - offset));
}
void X86Lowering::layout() {
  locals_.clear(); objects_.clear(); phiTemps_.clear();
  cursor_ = 0; outgoing_ = 0;
  auto spill = [this](const std::string& key, const Type& ty) {
    if (!locals_.emplace(key, Slot{reserve(8),ty}).second)
      throw std::runtime_error("duplicate SSA spill " + key);
  };
  for (const Value& param : function_->params) spill(param.text,param.type);
  for (const Block& b : function_->blocks) for (const Inst& i : b.instructions) {
    if (!i.dst.empty()) spill(i.dst,i.type);
    if (i.op == "phi") phiTemps_[i.dst] = reserve(8);
    if (i.op == "alloca") objects_[i.dst] = reserve(i.sourceType.size);
    if (i.op == "call") {
      unsigned ng=0,nf=0,ns=0;
      size_t limit = i.values.size();
      if (i.callee == "llvm.memcpy.p0.p0.i64" || i.callee == "llvm.memset.p0.i64")
        limit = 3;
      for (size_t ai=0;ai<limit;++ai) {
        if (i.values[ai].type.isFloat()) { if (nf++ >= 8) ++ns; }
        else { if (ng++ >= 6) ++ns; }
      }
      outgoing_ = std::max(outgoing_, static_cast<uint64_t>(ns) * 8);
    }
  }
  frame_ = alignUp(cursor_ + outgoing_,16);
}
void X86Lowering::loadGP(const Value& v, const std::string& reg) {
  const bool w = wide(v.type);
  const std::string r = "%" + (w ? reg : reg32(reg));
  if (v.text[0] == '%') {
    emit(std::string(w ? "movq " : "movl ") + slot(v.text) + ", " + r);
  } else if (v.text[0] == '@') {
    if (v.type.name != "ptr") throw std::runtime_error("global symbol must have ptr type");
    emit("leaq " + gasSymbol(symbolName(v.text)) + "(%rip), %" + reg);
  } else if (v.text == "null" || v.text == "false") {
    emit("xor" + std::string(w?"q":"l") + " " + r + ", " + r);
  } else {
    uint64_t bits = 0;
    if (v.text == "true") bits = 1;
    else if (v.type.name == "float") bits = floatBits(v.text);
    else if (v.type.name == "double") bits = doubleBits(v.text);
    else bits = static_cast<uint64_t>(std::stoll(v.text, nullptr, 0));
    if (w) emit("movabsq $" + std::to_string(bits) + ", %" + reg);
    else emit("movl $" + std::to_string(static_cast<uint32_t>(bits)) + ", " + r);
  }
}
void X86Lowering::storeGP(const std::string& dst, const Type& ty, const std::string& reg) {
  emit(std::string(wide(ty)?"movq %":"movl %") +
       (wide(ty)?reg:reg32(reg)) + ", " + slot(dst));
}
void X86Lowering::loadXMM(const Value& v, const std::string& reg) {
  if (v.text[0] == '%') {
    emit(std::string(v.type.name=="double" ? "movsd " : "movss ") +
         slot(v.text) + ", %" + reg);
  } else {
    loadGP(v,"rax");
    emit(std::string(v.type.name=="double" ? "movq %rax, %" : "movd %eax, %") + reg);
  }
}
void X86Lowering::storeXMM(const std::string& dst, const Type& ty, const std::string& reg) {
  emit(std::string(ty.name=="double" ? "movsd %" : "movss %") + reg + ", " + slot(dst));
}
void X86Lowering::incoming() {
  unsigned ng=0,nf=0,ns=0;
  for (const Value& p : function_->params) {
    std::string dest = slot(p.text);
    bool fp = p.type.isFloat();
    unsigned index = fp ? nf++ : ng++;
    if (fp && index < 8) {
      emit(std::string(p.type.name == "double" ? "movsd %" : "movss %") +
           fpArgs[index] + ", " + dest);
    } else if (!fp && index < 6) {
      emit(std::string(wide(p.type) ? "movq %" : "movl %") +
           (wide(p.type) ? gpArgs[index] : reg32(gpArgs[index])) + ", " + dest);
    } else {
      std::string from = std::to_string(16 + ns++ * 8) + "(%rbp)";
      if (fp) {
        emit(std::string(p.type.name == "double"?"movsd ":"movss ") + from + ", %xmm0");
        emit(std::string(p.type.name == "double"?"movsd %xmm0, ":"movss %xmm0, ") + dest);
      } else {
        emit(std::string(wide(p.type)?"movq ":"movl ") + from +
             (wide(p.type)?", %rax":", %eax"));
        storeGP(p.text,p.type,"rax");
      }
    }
  }
}
void X86Lowering::copies(const std::string& source, const std::string& destination) {
  const Block* target = nullptr;
  for (const Block& b : function_->blocks)
    if ("%" + b.label == destination) { target=&b; break; }
  if (!target) throw std::runtime_error("missing phi destination block");
  for (const Inst& i : target->instructions) {
    if (i.op != "phi") break;
    const PhiEdge* incomingEdge=nullptr;
    for (const PhiEdge& e : i.incoming)
      if (e.predecessor == source) { incomingEdge=&e; break; }
    if (!incomingEdge) throw std::runtime_error("missing phi edge");
    loadGP({i.type,incomingEdge->source},"rax");
    emit(std::string(wide(i.type) ? "movq %rax, " : "movl %eax, ") +
         loc(phiTemps_.at(i.dst)));
  }
  for (const Inst& i : target->instructions) {
    if (i.op != "phi") break;
    emit(std::string(wide(i.type) ? "movq " : "movl ") + loc(phiTemps_.at(i.dst)) +
         (wide(i.type) ? ", %rax" : ", %eax"));
    storeGP(i.dst,i.type,"rax");
  }
}
void X86Lowering::branch(const Inst& i, const std::string& source) {
  if (i.values.empty()) {
    copies(source, i.target);
    emit("jmp " + blockLabel(i.target));
    return;
  }
  loadGP(i.values[0],"rax");
  std::string otherwise = uniqueLabel();
  emit("testl %eax, %eax");
  emit("je " + otherwise);
  copies(source,i.target);
  emit("jmp " + blockLabel(i.target));
  out_ << otherwise << ":\n";
  copies(source,i.alternate);
  emit("jmp " + blockLabel(i.alternate));
}
void X86Lowering::call(const Inst& i) {
  const bool cp = i.callee == "llvm.memcpy.p0.p0.i64";
  const bool ms = i.callee == "llvm.memset.p0.i64";
  std::string callee = cp ? "memcpy" : (ms ? "memset" : i.callee);
  size_t argc = cp || ms ? 3 : i.values.size();
  unsigned ng=0,nf=0,ns=0;
  struct Position { bool fp, stack; unsigned index; };
  std::vector<Position> positions;
  for (size_t a=0;a<argc;++a) {
    bool fp=i.values[a].type.isFloat();
    unsigned index=fp?nf++:ng++;
    bool stack=index >= (fp?8U:6U);
    if (stack) index=ns++;
    positions.push_back({fp,stack,index});
  }
  for (size_t a=0;a<argc;++a) {
    if (!positions[a].stack) continue;
    const Value& v=i.values[a];
    std::string addr=std::to_string(8*positions[a].index)+"(%rsp)";
    if (v.type.isFloat()) {
      loadXMM(v,"xmm0");
      emit(std::string(v.type.name == "double" ? "movsd %xmm0, " : "movss %xmm0, ") + addr);
    } else {
      loadGP(v,"rax");
      emit(std::string(wide(v.type)?"movq %rax, ":"movl %eax, ") + addr);
    }
  }
  unsigned fpRegisterCount = 0;
  for (size_t a=0;a<argc;++a) {
    if (positions[a].stack) continue;
    const Value& v=i.values[a];
    unsigned idx=positions[a].index;
    if (v.type.isFloat()) {
      loadXMM(v,fpArgs[idx]);
      fpRegisterCount=std::max(fpRegisterCount,idx+1);
    } else {
      loadGP(v,"rax");
      emit(std::string(wide(v.type)?"movq %rax, %":"movl %eax, %") +
           (wide(v.type)?gpArgs[idx]:reg32(gpArgs[idx])));
    }
  }
  bool variadic = false;
  for (const Function& fn : module_.functions)
    if (fn.name == i.callee) { variadic=fn.variadic; break; }
  if (variadic) emit("movb $" + std::to_string(fpRegisterCount) + ", %al");
  emit("call " + gasSymbol(callee));
  if (!i.dst.empty()) {
    if (i.type.isFloat()) storeXMM(i.dst,i.type,"xmm0");
    else storeGP(i.dst,i.type,"rax");
  }
}
void X86Lowering::function(const Function& f) {
  if (f.declaration) return;
  function_=&f;
  ++functionIndex_;
  labelIndex_=0;
  layout();
  std::string name=gasSymbol(f.name);
  out_ << ".text\n.globl " << name << "\n.type " << name << ", @function\n"
       << name << ":\n";
  emit("pushq %rbp");
  emit("movq %rsp, %rbp");
  if (frame_) {
    if (frame_ <= INT32_MAX) emit("subq $" + std::to_string(frame_) + ", %rsp");
    else {
      emit("movabsq $" + std::to_string(frame_) + ", %r11");
      emit("subq %r11, %rsp");
    }
  }
  incoming();
  for (const Block& b : f.blocks) {
    out_ << blockLabel(b.label) << ":\n";
    for (const Inst& i : b.instructions) {
      try { instruction(i,"%" + b.label); }
      catch (const std::exception& e) {
        throw std::runtime_error("line " + std::to_string(i.line) + ": " + e.what());
      }
    }
  }
  out_ << ".size " << name << ", .-" << name << '\n';
}
void X86Lowering::run() {
  out_ << ".section .note.GNU-stack,\"\",@progbits\n";
  for (const Global& g : module_.globals) global(g);
  for (const Function& f : module_.functions) function(f);
  if (!out_) throw std::runtime_error("assembly output write failure");
}
void lowerModule(const Module& m, const std::string& path) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot open assembly output: " + path);
  X86Lowering(m,out).run();
}
}  // namespace toy
