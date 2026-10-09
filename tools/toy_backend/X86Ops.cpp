#include "X86Lowering.h"
#include <map>
#include <stdexcept>

namespace toy {
namespace {
std::string suff(const Type& t) { return t.name == "i64" ? "q" : "l"; }
} // namespace
void X86Lowering::integer(const Inst& i) {
  const bool w = i.type.name == "i64";
  const std::string s = suff(i.type);
  const std::string a = w ? "%rax" : "%eax";
  const std::string b = w ? "%rcx" : "%ecx";
  loadGP(i.values.at(0),"rax");
  loadGP(i.values.at(1),"rcx");
  if (i.op == "sdiv" || i.op == "srem") {
    const std::string doDivide = uniqueLabel(), finish = uniqueLabel();
    emit("cmp" + s + " $-1, " + b);
    emit("jne " + doDivide);
    if (w) {
      emit("movabsq $9223372036854775808, %r10");
      emit("cmpq %r10, %rax");
    } else emit("cmpl $-2147483648, %eax");
    emit("jne " + doDivide);
    if (i.op == "srem") emit("xor" + s + " " + a + ", " + a);
    emit("jmp " + finish);
    out_ << doDivide << ":\n";
    emit(w ? "cqto" : "cltd");
    emit("idiv" + s + " " + b);
    if (i.op == "srem") emit("mov" + s + " " + (w ? "%rdx" : "%edx") + ", " + a);
    out_ << finish << ":\n";
  } else if (i.op == "shl" || i.op == "lshr" || i.op == "ashr") {
    const std::map<std::string,std::string> m = {{"shl","shl"},{"lshr","shr"},{"ashr","sar"}};
    emit(m.at(i.op) + s + " %cl, " + a);
  } else {
    const std::map<std::string,std::string> m = {
      {"add","add"},{"sub","sub"},{"mul","imul"},
      {"and","and"},{"or","or"},{"xor","xor"}};
    emit(m.at(i.op) + s + " " + b + ", " + a);
  }
  storeGP(i.dst,i.type,"rax");
}
void X86Lowering::floating(const Inst& i) {
  if (i.op == "fneg") {
    loadGP(i.values.at(0),"rax");
    emit("xorl $-2147483648, %eax");
    storeGP(i.dst,i.type,"rax");
    return;
  }
  loadXMM(i.values.at(0),"xmm0");
  loadXMM(i.values.at(1),"xmm1");
  const std::map<std::string,std::string> m = {
      {"fadd","addss"},{"fsub","subss"},{"fmul","mulss"},{"fdiv","divss"}};
  emit(m.at(i.op) + " %xmm1, %xmm0");
  storeXMM(i.dst,i.type,"xmm0");
}
void X86Lowering::cast(const Inst& i) {
  const Value& v = i.values.at(0);
  if (i.op == "sitofp") {
    loadGP(v,"rax");
    emit(std::string(v.type.name == "i64" ? "cvtsi2ssq %rax" : "cvtsi2ssl %eax") + ", %xmm0");
    storeXMM(i.dst,i.type,"xmm0");
  } else if (i.op == "fptosi") {
    loadXMM(v,"xmm0");
    emit(std::string(i.type.name == "i64" ? "cvttss2siq %xmm0, %rax" :
                    "cvttss2sil %xmm0, %eax"));
    storeGP(i.dst,i.type,"rax");
  } else if (i.op == "fpext") {
    loadXMM(v,"xmm0");
    emit("cvtss2sd %xmm0, %xmm0");
    storeXMM(i.dst,i.type,"xmm0");
  } else {
    loadGP(v,"rax");
    if (i.op == "sext") emit("movslq %eax, %rax");
    if (i.op == "zext" && v.type.name == "i32") emit("movl %eax, %eax");
    if (i.op == "zext" && v.type.name == "i1") emit("andl $1, %eax");
    storeGP(i.dst,i.type,"rax");
  }
}
void X86Lowering::compare(const Inst& i) {
  if (i.op == "icmp") {
    const bool w = i.values.at(0).type.name == "i64";
    loadGP(i.values.at(0),"rax");
    loadGP(i.values.at(1),"rcx");
    emit(std::string(w?"cmpq %rcx, %rax":"cmpl %ecx, %eax"));
    const std::map<std::string,std::string> predicates = {
      {"eq","e"},{"ne","ne"},{"slt","l"},{"sle","le"},{"sgt","g"},{"sge","ge"}};
    emit("set" + predicates.at(i.pred) + " %al");
  } else {
    loadXMM(i.values.at(0),"xmm0");
    loadXMM(i.values.at(1),"xmm1");
    emit("ucomiss %xmm1, %xmm0");
    const std::map<std::string,std::string> predicates = {
      {"oeq","e"},{"une","ne"},{"olt","b"},{"ole","be"},{"ogt","a"},{"oge","ae"}};
    emit("set" + predicates.at(i.pred) + " %al");
    if (i.pred == "oeq" || i.pred == "olt" || i.pred == "ole") {
      emit("setnp %dl");
      emit("andb %dl, %al");
    } else if (i.pred == "une") {
      emit("setp %dl");
      emit("orb %dl, %al");
    }
  }
  emit("movzbl %al, %eax");
  storeGP(i.dst,i.type,"rax");
}
void X86Lowering::instruction(const Inst& i, const std::string& block) {
  const std::string& op = i.op;
  if (op == "phi") return;
  if (op == "alloca") {
    emit("leaq " + loc(objects_.at(i.dst)) + ", %rax");
    storeGP(i.dst,i.type,"rax");
  } else if (op == "load") {
    loadGP(i.values.at(0),"rax");
    if (i.type.name == "float" || i.type.name == "double") {
      emit(std::string(i.type.name=="float"?"movss ":"movsd ") + "(%rax), %xmm0");
      storeXMM(i.dst,i.type,"xmm0");
    } else {
      emit(std::string(i.type.name=="i1"||i.type.name=="i8"?"movzbl ":
            wide(i.type)?"movq ":"movl ") + "(%rax), " +
           (wide(i.type) ? "%rcx" : "%ecx"));
      storeGP(i.dst,i.type,"rcx");
    }
  } else if (op == "store") {
    Type ty=i.values.at(0).type;
    if (ty.isFloat()) {
      loadXMM(i.values.at(0),"xmm0");
      loadGP(i.values.at(1),"rax");
      emit(std::string(ty.name=="float"?"movss %xmm0, ":"movsd %xmm0, ")+"(%rax)");
    } else {
      loadGP(i.values.at(1),"rax");
      loadGP(i.values.at(0),"rcx");
      if (ty.name=="i1"||ty.name=="i8") emit("movb %cl, (%rax)");
      else emit(std::string(wide(ty)?"movq %rcx, ":"movl %ecx, ")+"(%rax)");
    }
  } else if (op == "getelementptr") {
    loadGP(i.values.at(0),"rax");
    loadGP(i.values.at(1),"r11");
    if (i.sourceType.size > 1) {
      if (i.sourceType.size <= INT32_MAX)
        emit("imulq $" + std::to_string(i.sourceType.size) + ", %r11, %r11");
      else {
        emit("movabsq $" + std::to_string(i.sourceType.size) + ", %r10");
        emit("imulq %r10, %r11");
      }
    }
    emit("addq %r11, %rax");
    storeGP(i.dst,i.type,"rax");
  } else if (op == "icmp" || op == "fcmp") {
    compare(i);
  } else if (op == "ret") {
    if (!i.values.empty()) {
      const Value& v=i.values.front();
      if (v.type.isFloat()) loadXMM(v,"xmm0");
      else loadGP(v,"rax");
    }
    emit("leave");
    emit("ret");
  } else if (op == "br") {
    branch(i,block);
  } else if (op == "unreachable") {
    emit("ud2");
  } else if (op == "call") {
    call(i);
  } else if (op == "select") {
    loadGP(i.values.at(0),"rax");
    const std::string otherwise=uniqueLabel(), finish=uniqueLabel();
    emit("testl %eax, %eax");
    emit("je " + otherwise);
    loadGP(i.values.at(1),"rax");
    storeGP(i.dst,i.type,"rax");
    emit("jmp " + finish);
    out_ << otherwise << ":\n";
    loadGP(i.values.at(2),"rax");
    storeGP(i.dst,i.type,"rax");
    out_ << finish << ":\n";
  } else if (op == "sitofp" || op == "fptosi" || op == "fpext" ||
             op == "sext" || op == "zext" || op == "trunc") {
    cast(i);
  } else if (op == "fneg" || op == "fadd" || op == "fsub" ||
             op == "fmul" || op == "fdiv") {
    floating(i);
  } else {
    integer(i);
  }
}
} // namespace toy
