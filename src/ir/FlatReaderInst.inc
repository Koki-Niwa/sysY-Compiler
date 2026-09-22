// ============================================================================
// ir/FlatReaderInst.inc —— `Reader::parseInstLine` 的实现
//
//   **由 `FlatReader.cpp` 在匿名命名空间内部 include**（不是独立编译单元）：
//   `Reader` 定义在那个匿名命名空间里，"成员函数的定义"必须与类同处一个
//   翻译单元。切分的目的只是 §C4 的单文件行数上限（600 行）。
//
//   规格：`FlatDump.cpp` 的 `printInst`（**打印与识别逐字对应**）。
//   形状：`[%N = ] <op> <随 opcode 变化的记号…> [@line K]`。
//   操作数一律按"**找下一个 `%N`**"取（打印器在每个操作数前都会打类型，
//   写死下标极易错位 —— 实测踩过）。
// ============================================================================
// ── 指令行解析（本文件最大的一块逻辑）────────────────────────────────────
//   规格：`FlatDump.cpp` 的 `printInst`（**打印与识别逐字对应**）。
void Reader::parseInstLine(Module& m, Function& f, BasicBlock* bb, const std::string& ln,
                           size_t lineNo) {
  std::vector<std::string> tok;
  detail::tokenizeLine(ln, tok);
  if (tok.empty()) return;
  size_t k = 0;
  uint32_t resultId = 0;
  bool hasResult = false;
  if (tok[0][0] == '%') {
    if (tok.size() < 3 || tok[1] != "=") { err(lineNo, "指令行格式错误（缺 `=`）"); return; }
    if (!detail::parseValueRef(tok[0], resultId)) { err(lineNo, "结果名无法解析"); return; }
    hasResult = true;
    k = 2;
  }
  const std::string opName = tok[k];
  Opcode op = Opcode::Nop;
  if (!opcodeFromName(opName, op)) {
    err(lineNo, "未知 opcode：`" + opName + "`（不在 iset.txt 里）");
    return;
  }
  ++k;
  const SourceLoc loc = detail::lineFromTokens(tok, k);

  auto val = [&](size_t idx) -> Value* {
    if (idx >= tok.size()) return nullptr;
    uint32_t id = 0;
    if (!detail::parseValueRef(tok[idx], id)) return nullptr;
    return ref(id, lineNo, opName.c_str());
  };
  auto typed = [&](size_t idx, const Type** ty) -> Value* {
    if (idx >= tok.size()) return nullptr;
    size_t p = 0;
    const Type* t = parseTypeText(tok[idx], p);
    if (t == nullptr || p != tok[idx].size()) return nullptr;
    if (ty != nullptr) *ty = t;
    // 类型之后的**下一个 `%N`** 就是值（打印器在值与逗号之间还会打类型）
    for (size_t j = idx + 1; j < tok.size(); ++j) {
      uint32_t id = 0;
      if (detail::parseValueRef(tok[j], id)) return ref(id, lineNo, opName.c_str());
    }
    return nullptr;
  };

  auto typeAt = [&](size_t idx, const Type** ty) -> bool {
    if (idx >= tok.size()) return false;
    size_t p = 0;
    const Type* t = parseTypeText(tok[idx], p);
    if (t == nullptr || p != tok[idx].size()) return false;
    if (ty != nullptr) *ty = t;
    return true;
  };
  auto label = [&](size_t idx) -> BasicBlock* {
    if (idx < tok.size() && tok[idx] == "label") ++idx;
    if (idx >= tok.size()) return nullptr;
    uint32_t bid = 0;
    if (!detail::parseBlockRef(tok[idx], bid)) return nullptr;
    return blockRef(m, f, tok, idx, lineNo);
  };
  // ── 记号游标（**"找下一个 `%N`"**，不做下标算术）───────────────────────
  //   为什么不用下标算术：打印器在**每个操作数前**都打一个类型
  //   （`store <ty> %v, ptr[T] %p`、`gep <ty>, ptr[T] %b, i64 %i`），
  //   于是"下一个操作数在第几个记号"随 opcode 变化。第一版用 `after(k+N)`
  //   硬算，结果 `store` 把**类型记号**当成了指针操作数 ——
  //   这正是"记号已经正确、却仍报操作数无法解析"的最后一层。
  //   ⇒ 规则改成：**操作数就是 `%N` 记号**，从当前位置往后找到第一个 `%N`
  //     就是下一个操作数（类型/逗号/`label` 一律跳过）。
  auto nextVal = [&](size_t from) -> Value* {
    for (size_t j = from; j < tok.size(); ++j) {
      uint32_t id = 0;
      if (detail::parseValueRef(tok[j], id)) return ref(id, lineNo, opName.c_str());
    }
    return nullptr;
  };

  // 【后置】从 `from` 起第 `n` 个（1 基）`%N` 的值；不足 ⇒ nullptr。
  //   ⚠️ 不能用 `nextVal(k+1)` 取"第二个操作数"：那只是把**搜索起点**后移一位，
  //   而 `nextVal` 找的是"从起点起的**第一个**" ⇒ 两次调用会返回**同一个值**
  //   （实测：`%12 = icmp eq i32 %11, %10` 读回来变成 `%11, %11`）。
  auto nthVal = [&](size_t from, int n) -> Value* {
    int seen = 0;
    for (size_t j = from; j < tok.size(); ++j) {
      uint32_t id = 0;
      if (!detail::parseValueRef(tok[j], id)) continue;
      if (++seen == n) return ref(id, lineNo, opName.c_str());
    }
    return nullptr;
  };

  Instruction* inst = nullptr;
  switch (op) {
    case Opcode::Ret:
      // `ret void` / `ret <ty> %v`：非 void 时**值在类型之后**，
      // 用 "找下一个 `%N`" 而不是写死下标（与其它 opcode 同一规则）。
      if (k < tok.size() && tok[k] == "void") {
        inst = createRet(nullptr, loc);
      } else {
        size_t j = k;
        while (j < tok.size()) {
          uint32_t id = 0;
          if (detail::parseValueRef(tok[j], id)) break;
          ++j;
        }
        const uint32_t rid = (j < tok.size()) ? [&] {
          uint32_t x = 0;
          detail::parseValueRef(tok[j], x);
          return x;
        }() : 0u;
        Value* v = (j < tok.size()) ? ref(rid, lineNo, opName.c_str()) : nullptr;
        inst = createRet(v, loc);
        if (v == nullptr && j < tok.size()) {
          // 记**文本号**：回填时统一 `mapId`（与 `deferred_` 同一口径）。
          OperandFixup fx;
          fx.inst = inst;
          fx.id = rid;
          fx.lineNo = lineNo;
          fx.what = "ret";
          fixups_.push_back(fx);
        }
      }
      break;
    case Opcode::Br:
      if (k < tok.size() && tok[k] == "i1") {
        // ⚠️ 条件值要按**"找下一个 `%N`"**取：`val(k+1)` 写死了下标，
        //   而 k+1 未必正好是那个值记号 ⇒ 条件读成 nullptr ⇒ 报
        //   "条件 br 的分支无法解析"。`nextVal` 是全文件统一的取值口径。
        Value* c = nthVal(k, 1);
        // `br i1 %c, label L1, label L2`。
        // ★★ `k` 是**操作数起点**（`br` 的 `i1` 就在 k），**不是** opcode 下标
        //   —— 第一版按"opcode 在 k-1"写成 `k+2`，于是 j 落到 `L1` 上：
        //   `label(j)` 拿到 `L1`（t 侥幸对了），接着"找逗号"从同一个 j 起走、
        //   立刻停住 ⇒ e 又拿到 `L1` 或 `label` ⇒ 每条条件分支都读不回来
        //   （实测 126 个文件报"条件 br 的分支无法解析"，而**只有**不带条件
        //   分支的文件能往返成功）。这一处同时踩了"下标差一"与
        //   "`label` 关键字没跳过"两个坑。
        size_t j = k + 3;                       // 起点：`i1 %c` 之后的那个逗号
        while (j < tok.size() && tok[j] == ",") ++j;
        BasicBlock* t = label(j);               // `label` 由 `label()` 跳过
        while (j < tok.size() && tok[j] != ",") ++j;
        if (j < tok.size()) ++j;                // 跳过 ","
        BasicBlock* e = label(j);
        if (c == nullptr || t == nullptr || e == nullptr) {
          err(lineNo, "条件 br 的分支无法解析");
          return;
        }
        inst = createCondBr(c, t, e, loc);
      } else {
        BasicBlock* t = label(k);   // 单向 `br L3`：标签就在 k
        if (t == nullptr) { err(lineNo, "br 的目标块无法解析"); return; }
        inst = createBr(t, loc);
      }
      break;
    case Opcode::Unreachable:
      inst = createUnreachable(loc);
      break;
    case Opcode::Alloca: {
      const Type* ty = nullptr;
      if (!typeAt(k, &ty)) { err(lineNo, "alloca 的类型无法解析"); return; }
      inst = createAlloca(ty, loc);
      break;
    }
    case Opcode::Load: {
      const Type* ty = nullptr;
      Value* p = typed(k, &ty);
      if (p == nullptr) { err(lineNo, "load 的操作数无法解析"); return; }
      inst = createLoad(ty, p, loc);
      break;
    }
    case Opcode::Store: {
      const Type* ty = nullptr;
      Value* v = typed(k, &ty);
      Value* p = nullptr;
      int seen = 0;
      for (size_t j = k; j < tok.size(); ++j) {
        uint32_t id = 0;
        if (!detail::parseValueRef(tok[j], id)) continue;
        if (++seen == 2) { p = ref(id, lineNo, opName.c_str()); break; }
      }
      if (v == nullptr || p == nullptr) { err(lineNo, "store 的操作数无法解析"); return; }
      inst = createStore(ty, v, p, loc);
      break;
    }
    case Opcode::GEP: {
      const Type* ety = nullptr;
      if (!typeAt(k, &ety)) { err(lineNo, "getelementptr 的元素类型无法解析"); return; }
      Value* base = nextVal(k);
      Value* idx = nullptr;
      {   // base 之后的**下一个** `%N` 就是下标
        bool seenBase = false;
        for (size_t j = k; j < tok.size(); ++j) {
          uint32_t id = 0;
          if (!detail::parseValueRef(tok[j], id)) continue;
          if (!seenBase) { seenBase = true; continue; }
          idx = ref(id, lineNo, opName.c_str());
          break;
        }
      }
      if (base == nullptr || idx == nullptr) {
        err(lineNo, "getelementptr 的操作数无法解析");
        return;
      }
      inst = createGEP(ety, base, idx, loc);
      break;
    }
    case Opcode::BitCast: {
      const Type* sty = nullptr;
      Value* v = typed(k, &sty);
      size_t toIdx = k + 2;                     // `k+1` 是值 `%v`
      while (toIdx < tok.size() && tok[toIdx] != "to") ++toIdx;
      if (v == nullptr || toIdx + 1 >= tok.size() || tok[toIdx] != "to") {
        err(lineNo, "bitcast 缺少 `to <类型>`");
        return;
      }
      size_t p = 0;
      const Type* dst = parseTypeText(tok[toIdx + 1], p);
      if (dst == nullptr || p != tok[toIdx + 1].size()) {
        err(lineNo, "bitcast 的目标类型无法解析");
        return;
      }
      inst = createBitCast(dst, v, loc);
      break;
    }
    case Opcode::ICmp:
    case Opcode::FCmp: {
      IPred ip = IPred::Eq;
      FPred fp = FPred::Oeq;
      const bool ok = (op == Opcode::ICmp) ? ipredFromName(tok[k], ip)
                                           : fpredFromName(tok[k], fp);
      if (!ok) { err(lineNo, "比较谓词无法识别：" + tok[k]); return; }
      Value* a = nthVal(k, 1);
      Value* b = nthVal(k, 2);
      if (a == nullptr || b == nullptr) { err(lineNo, "比较的操作数无法解析"); return; }
      inst = (op == Opcode::ICmp) ? createICmp(ip, a, b, loc) : createFCmp(fp, a, b, loc);
      break;
    }
    case Opcode::SIToFP:
    case Opcode::FPToSI:
    case Opcode::FPExt:
    case Opcode::SExt:
    case Opcode::ZExt:
    case Opcode::Trunc: {
      const Type* sty = nullptr;
      Value* v = typed(k, &sty);
      if (v == nullptr) { err(lineNo, "转换的操作数无法解析"); return; }
      inst = createCast(op, v, loc);
      break;
    }
    case Opcode::Call:
    case Opcode::LLVMMemCpy:
    case Opcode::LLVMMemSet: {
      const Type* retTy = nullptr;
      if (k < tok.size() && tok[k] == "void") {
        ++k;
      } else {
        size_t p = 0;
        retTy = k < tok.size() ? parseTypeText(tok[k], p) : nullptr;
        if (retTy == nullptr || p != tok[k].size()) {
          err(lineNo, "call 的返回类型无法解析");
          return;
        }
        ++k;
      }
      if (k >= tok.size() || tok[k].empty() || tok[k][0] != '@') {
        err(lineNo, "call 缺少被调函数名");
        return;
      }
      const std::string callee = tok[k].substr(1);
      ++k;
      std::vector<Value*> args;
      while (k < tok.size()) {
        if (tok[k] == "(" || tok[k] == ",") { ++k; continue; }
        if (tok[k] == ")") break;
        const Type* aty = nullptr;
        Value* av = typed(k, &aty);
        if (av == nullptr) { err(lineNo, "call 的实参无法解析：" + tok[k]); return; }
        args.push_back(av);
        k += 2;
      }
      inst = createCall(callee, retTy, args, loc);
      break;
    }
    case Opcode::Phi: {
      const Type* ty = nullptr;
      if (!typeAt(k, &ty)) { err(lineNo, "phi 的类型无法解析"); return; }
      ++k;
      std::vector<Value*> vals;
      std::vector<BasicBlock*> preds;
      // ⚠️ `[`/`]` **不会出现在记号里**（记号扫描器把 `[...]` 当类型整段吃，
      //   而 φ 的列表括号被它跳过）⇒ 循环的终止条件不能等 `]`，
      //   只能靠"**下一个记号不是 `(`**"（`@line` 或已到行尾）。
      while (k < tok.size()) {
        if (tok[k] == "[" || tok[k] == ",") { ++k; continue; }
        if (tok[k] == "]") break;
        if (tok[k] != "(" || k + 3 >= tok.size()) {
          if (tok[k] == "@line") break;        // 入值列表读完（`@line` 是行尾标记）
          err(lineNo, "phi 的入值必须是 `(%v LN)`");
          return;
        }
        uint32_t vid = 0;
        uint32_t bid = 0;
        if (!detail::parseValueRef(tok[k + 1], vid) || !detail::parseBlockRef(tok[k + 2], bid) ||
            tok[k + 3] != ")") {
          err(lineNo, "phi 的入值无法解析");
          return;
        }
        Value* v = ref(vid, lineNo, "phi");
        const size_t slot = vals.size();
        // ★ φ 的**前向引用**（回边/后面的 φ 当入值）必须能建出来：
        //   先放占位 nullptr，等 `finish()` 之后按**入值下标**回填。
        //   第一版在这里 `if (v == nullptr) return;` —— 于是**整条 φ 都没建**，
        //   结果"用了它的地方"全部报未定义（实测：`21_if_test2.sy` 的
        //   `%2 = phi [(%25 L10), (%5 L18)]`，其中 `%5` 是后面块里的 φ ⇒
        //   `%2` 被整条丢掉 ⇒ `ret %2` 报未定义）。
        //   ⚠️ 回填的位置必须记**下标**：φ 的入值与前驱块一一对应，
        //      "补到第一个空位"会把两条入值搞反。
        if (v == nullptr) {
          OperandFixup fx;
          fx.id = vid;
          fx.lineNo = lineNo;
          fx.what = "phi";
          fx.operandIdx = slot;
          fx.phiIdx = static_cast<long>(pendingPhis_.size());   // 本条 φ 即将 push 的下标
          fixups_.push_back(fx);
        }
        const auto bit = blocks_.find(bid);
        const bool blkMissing = (bit == blocks_.end() || bit->second == nullptr);
        preds.push_back(blkMissing ? nullptr : bit->second);
        vals.push_back(v);
        if (blkMissing) {
          OperandFixup bfx;
          bfx.id = 0;                 // 该位置没有值要回填
          bfx.lineNo = lineNo;
          bfx.what = "phi-block";
          bfx.operandIdx = slot;
          bfx.blockId = static_cast<long>(bid);
          bfx.phiIdx = static_cast<long>(pendingPhis_.size());
          fixups_.push_back(bfx);
        }
        k += 4;
      }
      // ★★ 指令**当场**建好并登记结果名 ★★
      //   "入值 + 前驱块"两样依赖后面的块，留到 `finish()` 补；但**结果名**
      //   必须现在就能查到 —— 否则同一函数里任何对这条 φ 的引用都解析不了
      //   （实测：条件分支 `br i1 %0` 里的 `%0` 正是一条 φ ⇒ 126 个文件
      //   读不回来）。占位入值用 `nullptr`，`finish()` 会逐个覆盖。
      Instruction* inst = createPhi(ty, std::vector<Value*>(vals.size(), nullptr),
                                    std::vector<BasicBlock*>(preds.size(), nullptr), loc);
      ownedModule_->ownInst(inst);
      if (hasResult) values_[mapId(resultId)] = inst;
      PendingPhi pp;
      pp.bb = bb;
      pp.inst = inst;
      pp.vals = std::move(vals);
      pp.preds = std::move(preds);
      pp.loc = loc;
      pp.lineNo = lineNo;
      pendingPhis_.push_back(std::move(pp));
      return;
    }
    case Opcode::Select: {
      Value* c = val(k);
      // `select i1 %c, <ty> %a, <ty> %b` ⇒ `%a` 是第一个值之后的**下一个** `%N`
      Value* a = nthVal(k, 2);      // 第 1 个是条件 `%c`
      Value* b = nthVal(k, 3);
      if (c == nullptr || a == nullptr || b == nullptr) {
        err(lineNo, "select 的操作数无法解析");
        return;
      }
      inst = createSelect(c, a, b, loc);
      break;
    }
    default: {
      const Type* ty = nullptr;
      if (!typeAt(k, &ty)) { err(lineNo, "指令的类型无法解析"); return; }
      Value* a = nextVal(k);
      if (a == nullptr) { err(lineNo, "指令的操作数无法解析"); return; }
      Value* b = nullptr;
      {   // 第二个 `%N` 存在 ⇒ 二元运算
        int seen = 0;
        for (size_t j = k; j < tok.size(); ++j) {
          uint32_t id = 0;
          if (!detail::parseValueRef(tok[j], id)) continue;
          if (++seen == 2) { b = ref(id, lineNo, opName.c_str()); break; }
        }
      }
      if (b != nullptr) {
        inst = createBin(op, a, b, loc);
        m.ownInst(inst);
      } else {
        inst = createInst(op, ty, loc);
        m.ownInst(inst);
        inst->addOperand(a);
      }
      break;
    }
  }
  if (inst == nullptr) return;
  // ★★ **两个登记点缺一不可**（这里曾经漏了第一个）★★
  //   * `m.ownInst(inst)` —— 进**模块的指令表**（`Module::insts_`）。
  //     `rebuildUseDef()` 是**遍历那张表**来重建反向边的 ⇒ 漏登记的指令
  //     在反向 use-def 里**根本不存在**，`users()` 恒空：
  //     症状是 FlatVerifier 报"反向 use-def 缺失"，而两个方向的**真实**数据
  //     其实一致（`00_main.sy` 那样的短函数只失败在"读回的产物"上）。
  //     这与"改了源码行为仍旧"是**同一类**：不是判据错，是**表没同步**。
  //   * `bb->addInst(inst)` —— 进基本块的指令序列（dump/遍历用）。
  m.ownInst(inst);
  bb->addInst(inst);
  if (hasResult) values_[mapId(resultId)] = inst;
}


