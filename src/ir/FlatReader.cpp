// ============================================================================
//   …（原有 2 行说明）
// ============================================================================
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/FlatDump.h"
#include "ir/FlatLex.h"
#include "support/Diagnostic.h"
#include "support/Errors.h"

namespace sysy {
namespace flat {
namespace {

// ============================================================================
// 读回器
// ============================================================================
class Reader {
 public:
  Reader(const std::string& text, DiagnosticEngine& diags) : text_(text), diags_(diags) {}

  Module* run() {
    splitLines();
    Module* m = new Module();
    ownedModule_ = m;
    size_t i = 0;
    while (i < lines_.size()) {
      const std::string& ln = lines_[i];
      if (ln.empty()) { ++i; continue; }
      if (ln[0] == '@') {
        parseGlobal(*m, ln, i + 1);
        ++i;
        continue;
      }
      if (ln.compare(0, 7, "define ") == 0) {
        // ⚠️ `parseFunction` 已经返回"**下一个待处理行**的下标" ⇒ 这里
        //   **不能**再 `++i`（主循环末尾那句是给普通行用的）。
        i = parseFunction(*m, i);
        continue;
      }
      err(i + 1, "无法识别的行（既不是全局、也不是 define）：" + ln.substr(0, 60));
      ++i;
    }
    // ★★ `finish()` 必须在 `bad_` 判断**之前**跑 ★★
    //   φ 是**延后**建的（它的入值要引用后面才出现的块），所以"函数体里
    //   引用了某个 φ 的名字"这件事在读到那一行时**还没定义** ——
    //   `values_` 里要等 `finish()` 才补齐。
    //   ⚠️ 原来写成 `if (bad_) { delete m; return nullptr; }` 在前 ⇒ 只要解析
    //   阶段出过**任何**一次错（哪怕只是"引用了尚未登记的 φ"这种自造成错），
    //   整个模块就被丢掉 ⇒ 读回产物为空 ⇒ 逐字节比对必然失败。
    //   症状极具误导性：报的是"引用了未定义的值 %14"，而 %14 就是那个 φ。
    // φ 的插入与回填**已经按函数做完了**（见 `parseFunction` 末尾）；
    //   这里只剩"非 φ 的前向引用"要回填，以及未定义引用的判定。
    // ⚠️ 前向引用的**回填**已经在 `parseFunction` 末尾按函数做完了
    //   （见 `resolveForwardRefs`）：内部号是全模块唯一的，拖到这里会让
    //   一条 fixup 匹配上**别的函数**里编号相同的指令，把它的操作数改错
    //   （实测：`%12 = icmp eq i32 %11, %10` 变成 `%11, %11`）。
    // 现在 `values_` 才是完整的 ⇒ 判定那些**真正**未定义的引用。
    // ⚠️ **未定义引用的判定已经按函数做完了**（见 `parseFunction` 的收尾）——
    //   拖到这里做是错的：`values_` 按函数清空，跑到这里只剩**最后一个函数**
    //   的值，前面函数的 φ/`ret` 全被误报"未定义"（实测 130 个文件）。
    if (bad_) { delete m; return nullptr; }
    m->rebuildCFG();
    m->rebuildUseDef();
    return m;
  }

 private:
  void splitLines() {
    std::string cur;
    for (char c : text_) {
      if (c == '\n') { lines_.push_back(cur); cur.clear(); continue; }
      if (c == '\r') continue;          // 容忍 CRLF（内容一致 ⇒ 往返仍逐字节）
      cur += c;
    }
    if (!cur.empty()) lines_.push_back(cur);
  }

  void err(size_t line, const std::string& msg) {
    bad_ = true;
    diags_.report(DiagLevel::Error, SourceLoc(static_cast<uint32_t>(line), 0),
                  "E-FLAT-READ", msg);
  }

  static std::string trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
  }

  // ── 全局：`@g = global <ty> zeroinitializer @line 2` ────────────────────
  void parseGlobal(Module& m, const std::string& raw, size_t lineNo) {
    const std::string ln = trim(raw);
    const size_t sp = ln.find(' ');
    if (sp == std::string::npos) { err(lineNo, "全局行缺少 `=`"); return; }
    const std::string name = ln.substr(1, sp - 1);
    std::vector<std::string> tok;
    detail::tokenizeLine(ln.substr(sp + 1), tok);
    // tok = [=, global, <ty...>, <init...>, (@line, N)?]
    if (tok.size() < 3 || tok[0] != "=" || tok[1] != "global") {
      err(lineNo, "全局行必须是 `@name = global <类型> <初始化器>`");
      return;
    }
    size_t k = 2;
    size_t pos = 0;
    const Type* ty = parseTypeText(tok[k], pos);
    if (ty == nullptr || pos != tok[k].size()) {
      err(lineNo, "全局对象类型无法解析：" + tok[k]);
      return;
    }
    ++k;
    GlobalVariable* g = m.createGlobal(name, ty);
    g->setLoc(detail::lineFromTokens(tok, k));
    m.globalAddr(g);   // 预建地址值（**顺序 = 全局声明顺序**，与 dump 一致）
    GlobalVariable::Data data;
    if (k < tok.size() && tok[k] == "zeroinitializer") {
      g->setZero();
      return;
    }
    if (k >= tok.size() || tok[k] != "{") { err(lineNo, "全局初始化器无法识别"); return; }
    ++k;   // `{`
    while (k < tok.size() && tok[k] != "}") {
      if (tok[k] == ",") { ++k; continue; }
      const std::string& offTok = tok[k];
      int64_t off = 0;
      if (!detail::parseI64(offTok, off)) { err(lineNo, "全局初始化器的偏移无法解析：" + offTok); return; }
      ++k;
      if (k >= tok.size() || tok[k] != "=") { err(lineNo, "全局初始化器缺少 `=`"); return; }
      ++k;
      if (k >= tok.size()) { err(lineNo, "全局初始化器缺少值"); return; }
      int64_t val = 0;
      if (!detail::parseI64(tok[k], val)) { err(lineNo, "全局初始化器的值无法解析：" + tok[k]); return; }
      ++k;
      data.emplace_back(static_cast<uint64_t>(off), static_cast<uint32_t>(val));
    }
    g->setInitData(std::move(data));
  }

#include "ir/FlatReaderFunc.inc"
  Function* f = m.createFunction(tok[2].substr(1), retTy);
    f->setLoc(SourceLoc());
    // ★ 块**一次建齐**（按标签顺序 ⇒ 索引 == 标签号）：前向引用会抢先建块，
    //   否则索引与 `L<n>` 错位（见 `prescanBlocks` 的说明）。
    prescanBlocks(m, f, start, endOfBody);
    //   块表为空时 `entry` 由 `f->entry()`（= 第 0 块）承担 —— 见下面的 `entry()`。
    auto entryOf = [&]() -> BasicBlock* { return f->entry(); };

    std::vector<uint32_t> paramIds;
    for (size_t k = 3; k < tok.size(); ++k) {
      if (tok[k] == "(" || tok[k] == ",") continue;
      if (tok[k] == ")") break;
      if (k + 1 >= tok.size()) { err(start + 1, "参数表不完整"); break; }
      size_t p2 = 0;
      const Type* pt = parseTypeText(tok[k], p2);
      uint32_t id = 0;
      if (pt == nullptr || p2 != tok[k].size() || !detail::parseValueRef(tok[k + 1], id)) {
        err(start + 1, "参数无法解析：" + tok[k] + " " + tok[k + 1]);
        return start + 1;
      }
      Instruction* p = createParam(pt, SourceLoc());
      m.ownInst(p);
      f->addParam(p);
      values_[mapId(id)] = p;
      paramIds.push_back(id);
      ++k;
    }

    size_t i = start + 1;
    BasicBlock* cur = nullptr;      // 由第一个 `L<n>:` 标签设定
    bool sawBraceLine = false;
    for (; i < lines_.size(); ++i) {
      const std::string ln = trim(lines_[i]);
      if (ln.empty()) continue;
      if (ln == "}") {
        if (sawBraceLine) { err(i + 1, "意外的 `}`"); }
        // 返回"**下一个待处理行**的下标"（= `}` 的下一行）。
        //   调用点赋值后**直接 continue**（不经过主循环末尾的 `++i`）⇒
        //   语义就是"从这一行继续"。契约写在 `parseFunction` 的注释上。
        // ★★ 本函数的 φ **在这里**插进块首并回填入值 ★★
        //   必须在 `blocks_` 还活着的时候做（入值的前驱块要按本函数的块号查），
        //   而且做完就清 `pendingPhis_`（它的下标是**每函数**的）。
        // ★ 顺序**必须是**先 `finishFunction()`、后 `resolvePhiBlockFixups()` ★
        //   `finishFunction()` 会把"还没有前驱"的条目保守改写成"自己"
        //   （畸形输入的兜底），所以块前向引用要在它**之后**覆盖，
        //   否则会被它盖掉（实测：回边的槽位仍然是"自己"，V3 报
        //   "φ 的前驱 {L1 L0} vs 块前驱 {L3 L0}"）。
        finishFunction();
        resolvePhiBlockFixups();
        resolveForwardRefs();
        pendingPhis_.clear();
        // ⚠️ `fixups_` 里**非 φ** 的条目（`ret`/`br` 的前向引用）与 `deferred_`
        //   **不能**在这里清：它们的回填/判定要等全部函数读完（值表齐全）。
        fixups_.erase(std::remove_if(fixups_.begin(), fixups_.end(),
                                     [](const OperandFixup& f) { return f.phiIdx >= 0; }),
                      fixups_.end());
        return i + 1;
      }
      // 常量定义行（在第一条指令之前）：`%7 = i32 5 @line 3`
      if (ln[0] == '%' && isConstantLine(ln)) {
        // 分派规则（**只看一个字符**，不依赖子串比较）：
        //   全局地址行一定含 `@`（`%6 = ptr[i32] @b`），常量行一定不含
        //   （`%2 = i32 0 @line 6` 里的 `@line` 是**属性**，不是 `@` 开头的记号）。
        if (ln.find(" @") != std::string::npos && ln.find("ptr[") != std::string::npos) {
          parseGlobalAddrLine(*ownedModule_, ln, i + 1);
        } else {
          parseConstantLine(*ownedModule_, *f, ln, i + 1);
        }
        continue;
      }
      // 标签：`L3:`
      if (ln[0] == 'L' && ln.back() == ':') {
        uint32_t idx = 0;
        if (!detail::parseBlockRef(ln.substr(0, ln.size() - 1), idx)) {
          err(i + 1, "标签无法解析：" + ln);
          return i + 1;
        }
        // ⚠️ "重复标签"的判据只能是 **`blocks_` 里有没有这个键**。
        //   原来还写了一个 `idx < blocks_.size()` —— 那是把**块号的数值**与
        //   **块的个数**当成一回事（`blocks_` 是 `L<n> → 块` 的映射，
        //   两者毫无关系）。实测症状：第一个函数里的 `L0` 被判成"重复的标签"
        //   （因为 `blocks_.size()` 恰好是 1、而 `idx` 是 0）⇒ 读回直接失败。
        if (declared_.count(idx) != 0) {          // **只有"声明过两次"才是重复**
          err(i + 1, "重复的标签：L" + std::to_string(idx));
          return i + 1;
        }
        declared_.insert(idx);
        if (idx >= nextBlockIdx_) nextBlockIdx_ = idx + 1;
        // 前向引用已经假设过这个块 ⇒ **复用**它（不是新建）；否则现在建。
        if (blocks_.count(idx) != 0) {
          cur = blocks_[idx];
        } else {
          cur = m.createBlock(f);    // 入口块（L0）也在这里建
          blocks_[idx] = cur;
          cur->setIndex(idx);
        }
        continue;
      }
      if (ln[0] == 'L') { err(i + 1, "标签行必须以 `:` 结尾：" + ln.substr(0, 40)); continue; }
      // 指令
      if (cur == nullptr) {
        // 第一条标签之前就有指令：畸形输入（发射器总会先印 `L0:`）
        if (entryOf() == nullptr) { err(i + 1, "指令出现在任何标签之前"); continue; }
        cur = entryOf();
        blocks_[0] = cur;
      }
      parseInstLine(m, *f, cur, ln, i + 1);
    }
    err(lines_.size(), "函数体没有闭合的 `}`");
    return lines_.size();
  }

  // 函数头部"定义区"的行：
  //   * 常量：`%N = i32 <num>` / `%N = f32 <hex|nan>`
  //   * 全局地址：`%N = ptr[T] @g`（S06 加的物化形式，见 FlatDump.cpp）
  static bool isConstantLine(const std::string& ln) {
    const size_t eq = ln.find(" = ");
    if (eq == std::string::npos) return false;
    const std::string rhs = ln.substr(eq + 3);
    if (rhs.compare(0, 4, "i32 ") == 0 || rhs.compare(0, 4, "i64 ") == 0 ||
        rhs.compare(0, 4, "f32 ") == 0) {
      return true;
    }
    return rhs.compare(0, 4, "ptr[") == 0 && rhs.find('@') != std::string::npos;
  }

  // 【后置】把 φ 的**前驱块前向引用**填好。
  //   【为什么必须在 `parseFunction` 末尾做】`blocks_` 是**每函数**的命名空间
  //   （读到下一个 `define` 就清空），而 `finish()` 是在**整个模块**读完才跑
  //   —— 那时 `blocks_` 里只剩最后一个函数的块 ⇒ 拿 `L18` 去查会查到**别的
  //   函数**的同名块（实测：φ 的前驱被填成 L12，V3 报"前驱集合不同"）。
  //   ⇒ 解析点必须在"本函数的块表还活着"的时候。
  // 【后置】把本函数"前向引用"的操作数（`ret`/`br` 的值）填好。**按函数**做：
  //   拖到全模块读完再扫，一条 fixup 会匹配上编号相同的**别的函数**的指令
  //   （实测把 icmp 的第二个操作数改成了第一个）。
  void resolveForwardRefs() {
    for (const OperandFixup& fx : fixups_) {
      if (fx.phiIdx >= 0 || fx.inst == nullptr) continue;   // φ 有自己的通道
      if (fx.id == OperandFixup::kNoValueFixup) continue;   // 只填块，没有值
      const auto it = values_.find(mapId(fx.id));
      if (it == values_.end()) continue;
      if (fx.operandIdx != static_cast<size_t>(-1)) {
        fx.inst->setOperand(fx.operandIdx, it->second);
      } else if (fx.inst->numOperands() == 0) {
        fx.inst->addOperand(it->second);
      } else {
        fx.inst->setOperand(0, it->second);
      }
    }
  }

  void resolvePhiBlockFixups() {
    for (const OperandFixup& fx : fixups_) {
      if (fx.blockId < 0 || fx.phiIdx < 0) continue;
      const auto bit = blocks_.find(static_cast<uint32_t>(fx.blockId));
      if (bit == blocks_.end() || bit->second == nullptr) continue;
      if (static_cast<size_t>(fx.phiIdx) >= pendingPhis_.size()) continue;
      Instruction* inst = pendingPhis_[static_cast<size_t>(fx.phiIdx)].inst;
      if (inst != nullptr) inst->setSucc(fx.operandIdx, bit->second);
    }
  }

  // ── 定义区的两种行（由 `parseFunction` 按前缀分派，互不干扰）──────────
//   …（原有 2 行说明）
  //      分成两个函数之后，分派是**调用点**的事，不再依赖函数内的控制流。
  void parseConstantLine(Module& m, Function& /*f*/, const std::string& ln, size_t lineNo) {
    const size_t eq = ln.find(" = ");
    uint32_t id = 0;
    if (!detail::parseValueRef(ln.substr(0, eq), id)) { err(lineNo, "常量行缺少结果名"); return; }
    const std::string rhs = ln.substr(eq + 3);
    std::vector<std::string> tok;
    detail::tokenizeLine(rhs, tok);
    if (tok.size() < 2) { err(lineNo, "常量行不完整"); return; }
    const SourceLoc loc = detail::lineFromTokens(tok, 2);
    size_t p = 0;
    const Type* ty = parseTypeText(tok[0], p);
    if (ty == nullptr || p != tok[0].size()) {
      err(lineNo, "常量类型无法解析：" + tok[0]);
      return;
    }
    Instruction* c = m.createInst(ty->isFloat() ? Opcode::ConstantFP : Opcode::ConstantInt,
                                  ty, loc);
    m.ownInst(c);
    if (ty->isFloat()) {
      if (tok[1] == "nan") {
        c->setFloatBits(0x7fc00000u);
      } else {
        const float fv = static_cast<float>(std::strtod(tok[1].c_str(), nullptr));
        uint32_t bits = 0;
        std::memcpy(&bits, &fv, sizeof(bits));
        c->setFloatBits(bits);
      }
    } else {
      int64_t v = 0;
      if (!detail::parseI64(tok[1], v)) { err(lineNo, "常量值无法解析：" + tok[1]); return; }
      c->setIntBits(v);
    }
    values_[mapId(id)] = c;
  }

  // `%N = ptr[T] @g` —— 全局地址的物化形式（见 FlatDump.cpp 的说明）。
  void parseGlobalAddrLine(Module& m, const std::string& ln, size_t lineNo) {
    const size_t eq = ln.find(" = ");
    uint32_t id = 0;
    if (!detail::parseValueRef(ln.substr(0, eq), id)) {
      err(lineNo, "全局地址行缺少结果名");
      return;
    }
    const std::string rhs = ln.substr(eq + 3);
    const size_t at = rhs.find('@');
    if (at == std::string::npos) { err(lineNo, "全局地址行缺少 `@名字`"); return; }
    size_t endPos = std::string::npos;
    int depth = -1;
    for (size_t k = 0; k < rhs.size(); ++k) {
      if (rhs[k] == '[') { if (depth < 0) depth = 0; ++depth; }
      else if (rhs[k] == ']' && depth > 0) --depth;
      if (depth == 0) { endPos = k; break; }
    }
    if (endPos == std::string::npos) { err(lineNo, "全局地址行类型括号不配平"); return; }
    size_t p2 = 0;
    const Type* gty = parseTypeText(rhs.substr(0, endPos + 1), p2);
    if (gty == nullptr || !gty->isPtr()) { err(lineNo, "全局地址行的类型无法解析"); return; }
    std::string gn = rhs.substr(at + 1);
    while (!gn.empty() && (gn.back() == ' ' || gn.back() == '\t')) gn.pop_back();
    GlobalVariable* g = m.findGlobal(gn);
    if (g == nullptr) { err(lineNo, "全局地址引用了不存在的全局：@" + gn); return; }
    values_[mapId(id)] = m.globalAddr(g);
  }

  // 【后置】把 `%N` 解析成值（常量在别处已建好；找不到则报错）。
  Value* ref(uint32_t textId, size_t lineNo, const char* what) {
    const uint32_t id = mapId(textId);
    const auto it = values_.find(id);
    if (it == values_.end()) {
      // ⚠️ **不在这里报错**：φ 的名字要等 `finish()` 才登记（见那里的说明），
      //   所以"读到 `ret` 时它引用的 φ 还没定义"是**正常的前向引用**。
      //   这里只记下来，`run()` 末尾（finish 之后）统一判定 —— 那时还没定义
      //   的才是真的未定义。
      // ⚠️ 记**文本号**，判定时再翻一次：预扫是**按函数**做的，读到这一行时
      //   后面函数的 `%N` 还没进 `idMap_` ⇒ 当场翻出来的号以后会变
      //   （而重复引用同一个文本号会前后翻出不同的号）。
      deferred_.push_back({textId, lineNo, what});
      return nullptr;
    }
    return it->second;
  }

  // 【后置】解析一条指令行（**实现在 `FlatReaderInst.inc`**）。
  void parseInstLine(Module& m, Function& f, BasicBlock* bb, const std::string& ln,
                     size_t lineNo);

  BasicBlock* blockRef(Module& m, Function& f, const std::vector<std::string>& tok,
                       size_t idx, size_t lineNo) {
    if (idx >= tok.size()) { err(lineNo, "缺少标签参数"); return nullptr; }
    const std::string& s = tok[idx];
    if (s == "label") return blockRef(m, f, tok, idx + 1, lineNo);
    uint32_t bid = 0;
    if (!detail::parseBlockRef(s, bid)) { err(lineNo, "标签无法解析：" + s); return nullptr; }
    const auto it = blocks_.find(bid);
    if (it != blocks_.end() && it->second != nullptr) return it->second;
    // 前向引用（本关的发射顺序不会产生，但读回器要能处理）：先占位、后填。
    if (bid >= nextBlockIdx_) nextBlockIdx_ = bid + 1;
    BasicBlock* nb = (bid == 0) ? f.entry() : m.createBlock(&f);
    nb->setIndex(bid);
    blocks_[bid] = nb;
    return nb;
  }

 public:
  // ── 收尾：把 φ 与"编号 ↔ 指令"的对应补齐 ────────────────────────────────
  //   φ 为什么必须延后：它的入值里带**前驱块号**，而那些块的正文可能出现在
  //   φ 之后（回边就是这种形状）⇒ 必须等全部块都读完再建。
  // 【前置】只处理**当前** `pendingPhis_` 里那批（= 刚刚读完的那个函数）。
  //   【为什么按函数做】φ 的入值/前驱块要引用**本函数**的块与值；`blocks_`
  //   是每函数的命名空间，`finish()` 拖到全模块读完再跑就会查错块
  //   （实测：前驱被填成别的函数的同名块）。而且 `pendingPhis_` 也必须在
  //   函数结束时清掉，否则下标会跨函数串。
  void finishFunction() {
    for (const PendingPhi& p : pendingPhis_) {
      std::vector<BasicBlock*> preds = p.preds;
      for (BasicBlock*& pb : preds) {
        if (pb == nullptr) pb = p.bb;   // 畸形输入：保守指向自己（检查器会报红）
      }
      // 用**解析期就已经建好并登记过**的那条指令填两个字段：`parseInstLine`
      //   当场建 φ 并登记结果名（否则同一函数里对它的引用解析不了 —— 实测
      //   126 个文件报"条件 br 的分支无法解析"，那个"条件"就是一条 φ），
      //   只有"入值 + 前驱块"这两样（依赖后面的块）留到这里补。
      Instruction* inst = p.inst;
      for (size_t z = 0; z < p.vals.size(); ++z) {
        if (p.vals[z] != nullptr) inst->setOperand(z, p.vals[z]);   // nullptr = 待回填
      }
      for (size_t z = 0; z < preds.size(); ++z) {
        if (preds[z] != nullptr) inst->setSucc(z, preds[z]);   // nullptr = 待回填
      }
      // φ 必须排在**块首**（后端契约 4/V3）：此刻块里已有其它指令（可能还在
      //   φ 之前被读到）⇒ 不能 `addInst`（会插到终结符之后）。按已有 φ 数插入。
      size_t at = 0;
      while (at < p.bb->size() && p.bb->at(at) != nullptr &&
             p.bb->at(at)->op() == Opcode::Phi) {
        ++at;
      }
      p.bb->insertInst(at, inst);
      // ★ φ 的**前向引用入值**在这时回填（此刻 `values_` 已经齐全）
      const size_t thisIdx = static_cast<size_t>(&p - pendingPhis_.data());
      for (const OperandFixup& fx : fixups_) {
        if (fx.phiIdx < 0 || static_cast<size_t>(fx.phiIdx) != thisIdx) continue;
        const auto it = values_.find(mapId(fx.id));
        if (it != values_.end()) inst->setOperand(fx.operandIdx, it->second);
      }
    }
  }

 private:
  struct PendingPhi {
    BasicBlock* bb = nullptr;
    Instruction* inst = nullptr;   // 解析期就建好、已登记进 `values_`
    std::vector<Value*> vals;
    std::vector<BasicBlock*> preds;
    SourceLoc loc;
    size_t lineNo = 0;
  };
  Module* ownedModule_ = nullptr;
  const std::string& text_;
  DiagnosticEngine& diags_;
  std::vector<std::string> lines_;
  std::unordered_map<uint32_t, Value*> values_;    // `%N` → 值
  std::unordered_map<uint32_t, BasicBlock*> blocks_;  // `L<N>` → 块（含前向引用假设的）
  // 「这一行 `L<n>:` 真的出现过」——与 `blocks_` 分开：
  //   `br … label L1` 会**假设** L1 存在，真正的 `L1:` 行随后声明它。
  //   判"重复标签"必须看这个集合，不能看 `blocks_`（实测踩过）。
  std::unordered_set<uint32_t> declared_;
  // 文本号（dump 里印的 `%N`）→ 内部号（我们按打印顺序重排后的 0..n-1）
  std::unordered_map<uint32_t, uint32_t> idMap_;
  uint32_t nextReadId_ = 0;      // 全模块唯一的"读回内部号"计数器
  uint32_t nextBlockIdx_ = 0;                         // 下一个可用的块号
  std::vector<PendingPhi> pendingPhis_;
  struct DeferredRef {
    uint32_t id;
    size_t lineNo;
    std::string what;
  };
  std::vector<DeferredRef> deferred_;   // 待判定（finish 之后）的前向引用
  // ★ 待**回填操作数**的指令（`finish()` 之后才存在的值）。
  //   为什么不能只延后"报错"：`ret` 的操作数在**建指令时**就要用到
  //   （`createRet(v)` 的 v 决定它是 `ret void` 还是 `ret <ty> %v`），
  //   而它引用的 φ 要等 `finish()` 才存在 ⇒ 先建成"空操作数"，
  //   再在 finish 之后补上（`ret` 的操作数个数由**值**决定 ⇒ 用 addOperand）。
  struct OperandFixup {
    Instruction* inst = nullptr;
    // φ 的"前驱块"前向引用（回边/后面的块）也要延后解析：
    //   解析那行时 `blocks_` 里还没有那个标签 ⇒ 第一版把 `nullptr` 前驱
    //   保守改写成"自己"⇒ 与真实前驱集合不符（实测 V3 报
    //   "φ 的前驱 {L12 L10} vs 块前驱 {L18 L10}"）。
    long blockId = -1;
    // **文本号**（判定时再 `mapId`，见 `deferred_` 的说明）。
    //   ⚠️ 它同时兼任"有没有值要回填"的哨兵 ⇒ 必须用 `kNoValueFixup` 而不是 0：
    //   `%0` 是合法的值名，用 0 当哨兵会让"只填块前向引用"的那条顺手把
    //   `values_[mapId(0)]`（**某条 φ 自己**）回填到那个位置。
    //   实测：`%0 = phi [(%3 L0), (%10 L6)]` 读回来变成 `[(%3 L0), (%0 L6)]`
    //   ——149 个文件"往返不同"、198 个"引用了未定义的值"。
    static constexpr uint32_t kNoValueFixup = 0xFFFFFFFFu;
    uint32_t id = kNoValueFixup;
    size_t lineNo = 0;
    std::string what;
    // 回填的操作数下标（`size_t` 越界值 = "第一条缺操作数的指令"那个旧语义）。
    //   φ 的入值要**按位**回填（第 k 条入值对应第 k 个前驱块），
    //   不能"补到第一个空位上"。
    size_t operandIdx = static_cast<size_t>(-1);
    // ≥0 ⇒ 回填的是 `pendingPhis_[phiIdx]` 这条 φ 的入值。
    //   ⚠️ 存**下标**而不是指针：`pendingPhis_` 是 `vector`，`push_back`
    //   会让先前取到的元素指针失效 —— 这正是"持有容器内部指针"这一类 bug。
    long phiIdx = -1;
  };
  std::vector<OperandFixup> fixups_;
  bool bad_ = false;
};

// ── 指令行解析 ───────────────────────────────────────────────────────────
//   实现在 `FlatReaderInst.inc`（同一翻译单元内 include ⇒ 匿名命名空间的
//   成员函数定义合法）。切出去是因为它是本层最长的一块逻辑，
//   而 §C4 对 `compiler/src/**` 有 600 行的硬上限。
#include "ir/FlatReaderInst.inc"

}  // namespace

// ============================================================================
// 对外入口（`FlatDump.h`）
// ============================================================================
Module* parseFlatModule(const std::string& text, DiagnosticEngine& diags) {
  Reader r(text, diags);
  return r.run();
}

}  // namespace flat
}  // namespace sysy
