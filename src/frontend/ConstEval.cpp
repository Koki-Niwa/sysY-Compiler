// ============================================================================
// ConstEval.cpp —— 标量常量表达式求值（实现）
//
// 设计说明（为什么这么写）见 ConstEval.h 的文件头。这里只强调三件事：
//
// ① **求值是迭代的**（显式工作栈）。语料里 86_long_code2.sy 的表达式树深
//    4007，而 `1+1+1+…`（3 万个 `+`）是 prompt §九 点名的病态输入：
//    后序遍历用递归实现必然随树深增长 ⇒ 栈溢出。这里每个节点压一帧，
//    内存 O(树深)。
//
// ② **字面量解析只有这一份**（十进制/八进制/十六进制/十六进制浮点）。
//    S04 的初始化器降级必须调用 parseIntLit/parseFloatLit。
//
// ③ 归一化函数（normDivInt / normRemInt / satFptosi）是**对外可见**的：
//    S05 的 IRGen 必须产生与它们逐位一致的结果，否则"常量折叠"与
//    "运行时求值"会给出两个答案 —— 那种不一致在 490 个用例里极难定位。
// ============================================================================
#include "frontend/ConstEval.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "support/Diagnostic.h"

namespace sysy {

// ============================================================================
// 一、归一化（prompt §3.6；铁律 6：未定义行为必须规范化，不能听硬件的）
// ============================================================================
int32_t normDivInt(int32_t a, int32_t b) {
  // x/0 与 INT_MIN/-1 都是 UB：两个赛道行为不同（实测 aarch64 给 0、riscv 给 -1），
  // 所以统一归一化为 0。守卫（select）在 S05 插。
  if (b == 0) return 0;
  if (a == INT32_MIN && b == -1) return 0;
  return a / b;
}

int32_t normRemInt(int32_t a, int32_t b) {
  if (b == 0) return 0;
  if (a == INT32_MIN && b == -1) return 0;
  return a % b;
}

int32_t satFptosi(float v) {
  // NaN → 0；越界 → 饱和（与后端契约一致，**不要改**）。
  if (!(v == v)) return 0;                       // NaN
  const double d = static_cast<double>(v);
  if (d >= 2147483648.0) return INT32_MAX;
  if (d < -2147483648.0) return INT32_MIN;
  return static_cast<int32_t>(d);                // 截断（向零取整）
}

namespace {

// ── 字面量形状校验（**权威**）─────────────────────────────────────────────
bool allDigits(const std::string& s, size_t from, int base) {
  if (from >= s.size()) return false;
  for (size_t i = from; i < s.size(); ++i) {
    const char c = s[i];
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return false;
    if (v >= base) return false;
  }
  return true;
}

}  // namespace

// ============================================================================
// 二、字面量解析（**语义上的唯一权威**）
// ============================================================================
bool ConstEvaluator::parseIntLit(const IntLit& lit, int32_t& out, std::string& err) {
  const std::string& s = lit.text;
  err.clear();
  if (s.empty()) { err = "空整数字面量"; return false; }

  uint32_t acc = 0;
  size_t i = 0;
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (s.size() > 1 && s[0] == '0') {
    base = 8;
    i = 1;
  }
  if (!allDigits(s, i, base)) {
    // `09` / `0x` / `0xG` 之类：**必须报错**，绝不能当成 9 或 0
    // （S02 的语法层刻意接受它们，判定的责任在 S03 —— prompt §四）
    err = "非法的整数字面量 '" + s + "'";
    return false;
  }
  for (; i < s.size(); ++i) {
    const char c = s[i];
    const int d = (c >= '0' && c <= '9') ? (c - '0')
                : (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10);
    acc = acc * static_cast<uint32_t>(base) + static_cast<uint32_t>(d);   // 回绕
  }
  out = static_cast<int32_t>(acc);   // 二进制补码回绕（不产生 UB）
  return true;
}

bool ConstEvaluator::parseFloatLit(const FloatLit& lit, float& out, std::string& err) {
  const std::string& s = lit.text;
  err.clear();
  if (s.empty()) { err = "空浮点字面量"; return false; }
  // strtof 认 C99 的十六进制浮点（`0x1.921fb6p+1`）与十进制指数形式（`1e-6`）。
  // ⚠️ 它**不认** SysY 里没有的东西，但我们仍必须检查"整串都被消费"：
  //    否则 `0x1p`（缺指数）会被解析成 `0x1` 并把 `p` 留在原地 —— 那正是
  //    prompt §四点名要报错的情形之一。
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);  if (end == nullptr || end != s.c_str() + s.size()) {
    err = "非法的浮点字面量 '" + s + "'";
    return false;
  }
  out = v;   // 溢出到 inf 不报错（规范只说"应可表示"，实现上按 IEEE 语义走）
  return true;
}

// ============================================================================
// 三、求值（迭代后序）
// ============================================================================
namespace {

struct Frame {
  const Expr* e = nullptr;
  uint8_t state = 0;      // 0 = 刚进入；1 = 有一个子结果到达（在 acc 里）
  size_t idx = 0;         // 期望/刚到达的子节点序号
  ConstValue acc;         // 刚到达的子结果
  ConstValue lhs;         // 二元运算的左值（需要暂存）
  int64_t offset = 0;     // LVal 下标累加出的扁平偏移
};

bool isArithOp(TokKind k) {
  return k == TokKind::Plus || k == TokKind::Minus || k == TokKind::Star ||
         k == TokKind::Slash || k == TokKind::Percent;
}

ConstValue promote(const ConstValue& v, bool toFloat) {
  if (v.isFloat == toFloat) return v;
  return toFloat ? ConstValue::ofFloat(static_cast<float>(v.i)) : ConstValue::ofInt(satFptosi(v.f));
}

}  // namespace

bool ConstEvaluator::eval(const Expr& root, ConstValue& out, const char* code) {
  std::vector<Frame> st;
  st.push_back(Frame{&root, 0, 0, ConstValue(), ConstValue(), 0});
  ConstValue result;
  bool haveResult = false;

  // 报错并终止：编号 = code（一条错误只报一次，由调用点保证不重复调用）。
  auto fail = [&](const Expr& e, const std::string& msg) {
    if (diag_.errorCount() < kMaxDiagErrors) {
      diag_.report(DiagLevel::Error, e.loc, code, msg);
    }
    return false;
  };
  // 把一个完成的子结果交给栈顶父帧；栈空 ⇒ 这就是根的结果。
  auto deliver = [&](const ConstValue& v) {
    if (st.empty()) { result = v; haveResult = true; }
    else { st.back().acc = v; }
  };

  while (!st.empty()) {
    Frame f = st.back();
    st.pop_back();
    const Expr& e = *f.e;
    const std::string_view k = e.nodeKind();

    // ── 叶子：直接产生值 ────────────────────────────────────────────────
    if (k == "IntLit" || k == "FloatLit") {
      if (k == "IntLit") {
        const auto& lit = static_cast<const IntLit&>(e);
        int32_t v = 0;
        std::string err;
        if (!parseIntLit(lit, v, err)) return fail(e, err);
        deliver(ConstValue::ofInt(v));
      } else {
        const auto& lit = static_cast<const FloatLit&>(e);
        float v = 0.0f;
        std::string err;
        if (!parseFloatLit(lit, v, err)) return fail(e, err);
        deliver(ConstValue::ofFloat(v));
      }
      continue;
    }

    // ── state == 1：某个子结果到了 ──────────────────────────────────────
    if (f.state == 1) {
      if (k == "Unary") {
        const auto& n = static_cast<const Unary&>(e);
        const ConstValue v = f.acc;
        if (!v.isFloat && n.op == TokKind::Minus) {
          deliver(ConstValue::ofInt(static_cast<int32_t>(0u - static_cast<uint32_t>(v.i))));
        } else if (v.isFloat && n.op == TokKind::Minus) {
          deliver(ConstValue::ofFloat(-v.f));
        } else if (v.isFloat && n.op == TokKind::Plus) {
          deliver(v);
        } else if (!v.isFloat && n.op == TokKind::Plus) {
          deliver(v);
        } else {
          return fail(e, "`!` 不是常量表达式的一部分（规范 ConstExp -> AddExp）");
        }
        continue;
      }
      if (k == "Binary") {
        const auto& n = static_cast<const Binary&>(e);
        if (f.idx == 0) {                       // 左操作数到了：继续算右操作数
          Frame nf = f;
          nf.idx = 1;
          nf.lhs = f.acc;
          nf.state = 1;
          st.push_back(nf);
          if (n.rhs == nullptr) return fail(e, "二元运算缺少操作数");
          st.push_back(Frame{n.rhs.get(), 0, 0, ConstValue(), ConstValue(), 0});
          continue;
        }
        const ConstValue a = f.lhs, b = f.acc;
        const bool isFloatOp = (n.op != TokKind::Percent) && (a.isFloat || b.isFloat);
        const ConstValue x = promote(a, isFloatOp), y = promote(b, isFloatOp);
        if (n.op == TokKind::Percent) {
          if (a.isFloat || b.isFloat) {
            return fail(e, "`%` 的操作数必须是 int（浮点取余不在指令集里）");
          }
          deliver(ConstValue::ofInt(normRemInt(a.i, b.i)));
        } else if (isFloatOp) {
          float r = 0.0f;
          switch (n.op) {
            case TokKind::Plus:  r = x.f + y.f; break;
            case TokKind::Minus: r = x.f - y.f; break;
            case TokKind::Star:  r = x.f * y.f; break;
            case TokKind::Slash: r = x.f / y.f; break;
            default: return fail(e, "不是常量表达式允许的运算符");
          }
          deliver(ConstValue::ofFloat(r));
        } else {
          int32_t r = 0;
          switch (n.op) {
            case TokKind::Plus:  r = static_cast<int32_t>(static_cast<uint32_t>(a.i) + static_cast<uint32_t>(b.i)); break;
            case TokKind::Minus: r = static_cast<int32_t>(static_cast<uint32_t>(a.i) - static_cast<uint32_t>(b.i)); break;
            case TokKind::Star:  r = static_cast<int32_t>(static_cast<uint32_t>(a.i) * static_cast<uint32_t>(b.i)); break;
            case TokKind::Slash: r = normDivInt(a.i, b.i); break;
            default: return fail(e, "不是常量表达式允许的运算符");
          }
          deliver(ConstValue::ofInt(r));
        }
        continue;
      }
      if (k == "LVal") {
        const auto& n = static_cast<const LVal&>(e);
        // 到达的下标结果必须是 int 常量
        if (f.acc.isFloat) return fail(e, "数组下标必须是整型常量");
        const ConstObject* obj = env_.findConst(n.name);
        if (obj == nullptr) return fail(e, "'" + n.name + "' 不是已定义的符号常量");
        const Type* cur = obj->type;
        // 累加扁平偏移（行主序）：offset = offset * len + idx
        f.offset = f.offset * (cur->isArray() ? cur->len : 1) + f.acc.i;
        if (cur->isArray()) cur = cur->elem;
        if (f.idx + 1 < n.indices.size()) {
          Frame nf = f;
          nf.idx = f.idx + 1;
          nf.state = 1;
          st.push_back(nf);
          const Expr* sub = n.indices[nf.idx].get();
          if (sub == nullptr) return fail(e, "下标缺失");
          st.push_back(Frame{sub, 0, 0, ConstValue(), ConstValue(), 0});
          continue;
        }
        // 全部下标到齐：cur 必须是标量，offset 必须在范围内
        if (cur != nullptr && cur->isArray()) {
          return fail(e, "常量数组 '" + n.name + "' 的下标个数不足");
        }
        if (f.offset < 0 || f.offset >= static_cast<int64_t>(obj->elems.size())) {
          return fail(e, "常量数组 '" + n.name + "' 的下标越界");
        }
        deliver(obj->elems[static_cast<size_t>(f.offset)]);
        continue;
      }
      return fail(e, std::string("不是常量表达式（结点 ") + std::string(k) + "）");
    }

    // ── state == 0：进入节点 ────────────────────────────────────────────
    if (k == "Unary") {
      const auto& n = static_cast<const Unary&>(e);
      if (n.op != TokKind::Plus && n.op != TokKind::Minus) {
        return fail(e, "`!` 不是常量表达式的一部分（规范 ConstExp -> AddExp）");
      }
      if (n.operand == nullptr) return fail(e, "一元运算缺少操作数");
      st.push_back(Frame{&e, 1, 0, ConstValue(), ConstValue(), 0});
      st.push_back(Frame{n.operand.get(), 0, 0, ConstValue(), ConstValue(), 0});
      continue;
    }
    if (k == "Binary") {
      const auto& n = static_cast<const Binary&>(e);
      if (!isArithOp(n.op)) {
        return fail(e, "关系/相等/逻辑运算符不属于常量表达式（规范 ConstExp -> AddExp）");
      }
      if (n.lhs == nullptr || n.rhs == nullptr) return fail(e, "二元运算缺少操作数");
      st.push_back(Frame{&e, 1, 0, ConstValue(), ConstValue(), 0});
      st.push_back(Frame{n.lhs.get(), 0, 0, ConstValue(), ConstValue(), 0});
      continue;
    }
    if (k == "LVal") {
      const auto& n = static_cast<const LVal&>(e);
      const ConstObject* obj = env_.findConst(n.name);
      if (obj == nullptr) return fail(e, "'" + n.name + "' 不是已定义的符号常量");
      if (n.indices.empty()) {
        // 整个常量对象：只有标量能当值用
        if (obj->type != nullptr && obj->type->isArray()) {
          return fail(e, "常量数组 '" + n.name + "' 必须完全下标后才能取值");
        }
        if (obj->elems.empty()) return fail(e, "常量 '" + n.name + "' 没有值");
        deliver(obj->elems[0]);
        continue;
      }
      st.push_back(Frame{&e, 1, 0, ConstValue(), ConstValue(), 0});
      if (n.indices[0] == nullptr) return fail(e, "下标缺失");
      st.push_back(Frame{n.indices[0].get(), 0, 0, ConstValue(), ConstValue(), 0});
      continue;
    }
    return fail(e, std::string("不是常量表达式（结点 ") + std::string(k) + "）");
  }

  if (!haveResult) return false;
  out = result;
  return true;
}

// ============================================================================
// 四、维度求值（规范 §3 ConstDef 3：**非负整数**常量；§3 FuncFParam 2）
// ============================================================================
bool ConstEvaluator::evalDim(const Expr* e, int64_t& out, const char* code) {
  const char* c = (code != nullptr) ? code : "E-ARRAY-DIM";
  if (e == nullptr) {
    if (diag_.errorCount() < kMaxDiagErrors) {
      diag_.report(DiagLevel::Error, SourceLoc(0, 0), c,
                   "数组维度必须显式给出长度（只有形参第一维可以写成 `[]`）");
    }
    return false;
  }
  ConstValue v;
  if (!eval(*e, v, c)) return false;
  if (v.isFloat) {
    if (diag_.errorCount() < kMaxDiagErrors) {
      diag_.report(DiagLevel::Error, e->loc, c, "数组维度必须是整型常量表达式（不能是浮点）");
    }
    return false;
  }
  if (v.i < 0) {
    if (diag_.errorCount() < kMaxDiagErrors) {
      diag_.report(DiagLevel::Error, e->loc, c,
                   "数组维度必须是非负整数（规范 §3 ConstDef 3），实际是 " + std::to_string(v.i));
    }
    return false;
  }
  out = v.i;
  return true;
}

}  // namespace sysy
