// ============================================================================
// FlatReaderFunc.inc —— `Reader::parseFunction` 的**前半段**：预扫编号、
//   清函数级状态、解析形参。
//
//   **由 `FlatReader.cpp` 在匿名命名空间内部 include**（不是独立编译单元）：
//   它要用 `Reader` 的私有成员与 `detail::` 里的记号工具。
//   为什么拆出来：`check_line_budget.py` 的单文件上限是 600 行，而"读回器"
//   的职责（编号翻译 + 延后解析 + 两条定义区路径）确实塞不进一个文件。
// ============================================================================
  // ── 函数 ──────────────────────────────────────────────────────────────
  // 【后置】返回**下一个**待处理行的下标。
  // 【后置】预扫函数正文：把正文里**出现的每个 `%N` 定义**按出现顺序编号
  //   0,1,2,…，写进 `idMap_`。
  //   ★★ 为什么必须有这一步（这是读回器最关键的约定）★★
  //     打印器（`numberFunction`）按**打印顺序**发号：参数 → φ（块首）→
  //     各块的指令（常量在首次被引用时插进函数头部）。
  //     而读回器是**按文本顺序**建指令的，φ 还必须延后建（它的入值要引用
  //     后面才出现的块）—— 于是"创建顺序"与"打印顺序"不一致，
  //     直接沿用文本号会在**任何有 φ 的函数**上整体错位
  //     （实测：`17_div.sy` 的 `%14` vs `%12`，内容一样、只有编号不同）。
  //   ⇒ 读回时把"文本里的 `%N`"翻译成"我们自己的 0..n-1 号"，
  //     而 `dumpModule` 再按打印规则印回同样的号 ⇒ 往返逐字节相同。
  //   ⚠️ 号必须**模块级唯一**：`idMap_` 一旦按函数清空或从头发号，两个函数的
  //   `%2` 就会拿到**同一个内部号**，而 `values_` 是**按函数**清空的
  //   ⇒ 后一个函数会把前一个函数的值"顶掉"，先前那个函数里对它的引用
  //   就变成"未定义"（实测：`21_if_test2.sy` 的 `%2` 是一条 φ，被 `main`
  //   的 `%1` 顶掉后，`ifElseIf` 里的 `ret %2` 与另外两条 φ 全部报未定义）。
  //   ⇒ 预扫**全模块**、计数器只增不减，`mapId` 就是单射。
  void prescanIds(size_t start, size_t end) {
    // ★★ `idMap_` 必须**按函数**清空 ★★
    //   `%N` 是**每函数**的命名空间（`FlatDump::numberFunction` 每个函数从 0
    //   重新发号），内部号却要全模块唯一。第一版让 `idMap_` 跨函数累积 ⇒
    //   后一个函数里的 `%12` 被当成前一个函数那个 `%12` 的**同一个值**
    //   ⇒ 回填时把另一条指令塞进 φ 的入值（实测 `52_scope.sy`）。
    idMap_.clear();
    uint32_t next = nextReadId_;
    for (size_t i = start; i < end && i < lines_.size(); ++i) {
      const std::string ln = trim(lines_[i]);
      if (ln.empty()) continue;
      if (ln.compare(0, 7, "define ") == 0) {
        // 形参：`define <ty> @f(<ty> %N, …)`
        std::vector<std::string> tok;
        detail::tokenizeLine(ln, tok);
        for (const std::string& t : tok) {
          uint32_t id = 0;
          if (detail::parseValueRef(t, id) && idMap_.find(id) == idMap_.end()) {
            idMap_[id] = next++;
          }
        }
        continue;
      }
      if (ln[0] == '%') {
        const size_t eq = ln.find(" = ");
        if (eq == std::string::npos) continue;
        uint32_t id = 0;
        if (detail::parseValueRef(ln.substr(0, eq), id) && idMap_.find(id) == idMap_.end()) {
          idMap_[id] = next++;
        }
      }
    }
    nextReadId_ = next;
  }
  // 【后置】把文本号翻成内部号（未登记 ⇒ 原样，`ref()` 会报未定义）。
  uint32_t mapId(uint32_t textId) const {
    const auto it = idMap_.find(textId);
    return it == idMap_.end() ? textId : it->second;
  }

  // 【后置】按**标签顺序**把本函数的块一次建好。
  //   【为什么】`Module::createBlock` 按创建顺序追加、`reindexBlocks` 按追加
  //   顺序发号 ⇒ **先建哪个，哪个就是 L0/L1/…**。而"读到 `L<n>:` 才建"会被
  //   **前向引用**抢先（`br i1 %c, label L5, label L6` 先把 L5/L6 建出来）
  //   ⇒ 随后真正的 `L3:` 只能拿到更靠后的索引 ⇒ **索引与文本标签错位**。
  //   症状极难归因：往返能成功、只有号不同（189 个文件）—— 因为打印时按
  //   索引发块号，而索引已经不是标签。
  //   ⇒ 判据：块的索引**必须**等于它的文本标签号（`L<n>` 的 `n`）。
  size_t endOfBody = 0;   // 本函数正文的终点行（`prescanBlocks` 的入参）

  void prescanBlocks(Module& m, Function* f, size_t start, size_t end) {
    for (size_t q = start + 1; q < end && q < lines_.size(); ++q) {
      const std::string t = trim(lines_[q]);
      if (t.size() < 3 || t[0] != 'L' || t.back() != ':') continue;
      uint32_t bi = 0;
      if (!detail::parseBlockRef(t.substr(0, t.size() - 1), bi)) continue;
      if (blocks_.count(bi) != 0) continue;
      // ⚠️ 不能写 `(bi == 0 && f->blockCount() == 0) ? f->entry() : …`：
      //   `Function::entry()` 在**还没有块**时返回 `nullptr`（它不是"创建"），
      //   随后 `nb->setIndex()` 直接段错误（实测：`--from-flat` 一跑就 core）。
      //   `createBlock` 会按创建顺序追加 ⇒ 第一个建的就是 `blocks()[0]`
      //   （= `entry()`），所以这里一律用它，按标签顺序建就自然对齐。
      BasicBlock* nb = m.createBlock(f);
      nb->setIndex(bi);
      blocks_[bi] = nb;
      if (bi >= nextBlockIdx_) nextBlockIdx_ = bi + 1;
    }
  }

  size_t parseFunction(Module& m, size_t start) {
    const std::string& head = lines_[start];
    std::vector<std::string> tok;
    detail::tokenizeLine(head, tok);
    // tok = [define, <retTy>, @name, (, <pty>, %p, ,, ... , ), {]
    if (tok.size() < 4 || tok[0] != "define" || tok[2].empty() || tok[2][0] != '@') {
      err(start + 1, "define 行格式错误");
      return start + 1;
    }
    size_t pos = 0;
    const Type* retTy = parseTypeText(tok[1], pos);
    if (retTy == nullptr || pos != tok[1].size()) {
      err(start + 1, "函数返回类型无法解析：" + tok[1]);
      return start + 1;
    }
    // ★★ 块号 `L<n>` 是**每函数**的命名空间 ⇒ `blocks_`/`declared_` 必须清 ★★
    //   它们原来跨函数复用，于是第二个函数的 `L0` 被判成"重复的标签"，
    //   随后整个函数体被当成"函数体外的行"（实测：`01_var_defn2.sy`）。
    //   ⚠️ 但 `values_` **不能**清了：读回内部号（`idMap_`）现在是
    //   **全模块唯一**的（见 `prescanIds`），而 `%N` 的文本号在每个函数里
    //   各自从 0 开始 ⇒ 每个函数的 `%1` 都会翻到**同一个**内部号。
    //   按函数清空 `values_` 会让后一个函数把前一个函数的值"顶掉"，
    //   于是前一个函数里对它的引用（φ、`ret`）全变成"未定义"
    //   （实测：`21_if_test2.sy` 的 `%2` 是一条 φ，被 `main` 的 `%1` 顶掉）。
    blocks_.clear();
    declared_.clear();
    nextBlockIdx_ = 0;
    // ★★ 三张"延后处理"的表也必须按函数清空 ★★
    //   `pendingPhis_` / `fixups_` / `deferred_` 都以"本函数的 φ 下标"为索引
    //   或只在本函数内有意义；跨函数留着会让后一个函数的 `finish()` 去改
    //   **前一个函数**的 φ（实测：`main` 的 φ 下标 2 把 `ifElseIf` 第 2 条 φ
    //   的前驱覆盖成"自己"，V3 报"前驱集合不同 {L12 L10} vs {L18 L10}"）。
    //   ⚠️ `idMap_` **不在这里清**：它是全模块的翻译表（见 `prescanIds`）。
    // 预扫的终点：下一个 `define` 或文件末尾
    {
      size_t end = lines_.size();
      for (size_t q = start + 1; q < lines_.size(); ++q) {
        if (trim(lines_[q]).compare(0, 7, "define ") == 0) { end = q; break; }
      }
      prescanIds(start, end);
      endOfBody = end;   // 供 `prescanBlocks` 用（见 `parseFunction` 的调用点）
      // ★★ `values_` 按函数**整体清空**，再让下面的解析重新填 ★★
      //   为什么清是对的：`%N` 的文本号在每个函数里各自从 0 开始，而内部号
      //   （`idMap_`）是**全模块唯一**的 —— 于是每个函数都会**重新定义**
      //   自己的 `%1`（常量、全局地址、φ …），清掉不会与别的函数冲突。
      //   不清的后果：后一个函数的同名定义会覆盖前一个的绑定，前一个函数里
      //   剩下的引用就指向"已经不属于任何块"的旧指令（实测：读回后 V1 报
      //   "操作数指向一条不属于任何块的指令"）。
      //   ⚠️ 第一版想"只清本函数那一段号"，那会留下 `idMap_` 里指向已删值的
      //   映射 ⇒ 同样报 V1；整体清空才是自洽的（每个函数的定义都在自己的
      //   正文里重新出现）。
      values_.clear();
    }
  