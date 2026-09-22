// ============================================================================
// structured/FlattenLoops.cpp —— FlattenCFG 的**控制流容器展开**
//                            （`If` / `While` / `For`）
//
//   契约、φ 放置判据、临界边拆分的完整说明在 `FlattenCFG.h` 的文件头；
//   块骨架与遍历器在 `FlattenCFG.cpp`；单条 Op 的降级在 `FlattenLower.cpp`。
//   本文件只回答：**一个控制流容器展开成哪些块、φ 放在哪个块的什么位置**。
//
//   为什么单独一个文件：§C4 的单文件行数上限（600）。切分线是"职责"而不是
//   行数：这里全是**建块 + 连边 + 放 φ**，没有一条算术/内存 op 的降级逻辑。
// ============================================================================
#include <algorithm>

#include "structured/FlattenInternal.h"

namespace sysy {
namespace flat {

// ============================================================================
// `(If c) { then } { else }` →
//     [cur] --c--> [then] --→ [join] ←-- [else] ←-- [cur]
//   汇合块 join 的 φ 在**两个分支都展开完之后**才建（收尾帧）：
//   入边 = {then 的出口环境, else 的出口环境}，边块 = {thenB, elseB}。
// ============================================================================
Action FlatBuilder::lowerIf(Op* op, Frame& fr) {
  Value* c = map(op->operand(0));
  if (c == nullptr) {
    giveUp("If 的条件值缺失", op->loc);
    return Action::kStop;
  }
  // ⚠️ 先把 `fr` 用到的字段拷出来：`stack_.push_back` 可能让 `fr` 失效
  //    （它引用的是栈里的元素），压栈之后**不再碰 `fr`**。
  const bool inLoop = fr.inLoop;
  BasicBlock* loopExit = fr.loopExit;
  BasicBlock* loopHead = fr.loopHead;
  const Env pre = env_;
  Region* thenR = op->region(0);
  Region* elseR = op->region(1);

  BasicBlock* thenB = newBlock("then");
  BasicBlock* elseB = newBlock("else");
  BasicBlock* join = newBlock("join");
  if (failed_) return Action::kStop;
  cur_->addInst(createCondBr(c, thenB, elseB, op->loc));
  ownInst(cur_->back());

  const int thenSlot = allocFinishSlot();
  const int elseSlot = allocFinishSlot();
  Frame fthen;
  fthen.region = thenR;
  fthen.startBlock = thenB;
  fthen.cont = join;
  fthen.inLoop = inLoop;
  fthen.loopExit = loopExit;
  fthen.loopHead = loopHead;
  fthen.finishSlot = thenSlot;
  Frame felse = fthen;
  felse.region = elseR;
  felse.startBlock = elseB;
  felse.finishSlot = elseSlot;

  // 两个分支各拿**父环境的副本**（这就是"分支不互相污染"的落点）。
  enterBlock(thenB, pre);

  // 帧序（弹出顺序 = 执行顺序）：
  //   ① then 的 Region（此刻的 cur_ 已经是 thenB；走完会写 frameEnvs_[thenSeq]）
  //   ② **切到 elseB** 的收尾帧（不切的话 else 的指令会落进 thenB —— 实测症状：
  //      thenB 出现两条终结符、else 的内容跑到 then 里）
  //   ③ else 的 Region（走完写 frameEnvs_[elseSeq]）
  //   ④ 建汇合块的 φ（入边 = 两个分支的环境快照）
  Frame finElse;
  finElse.kind = FrameKind::Finish;
  finElse.finish = [this, elseB, pre]() { enterBlock(elseB, pre); };
  Frame finJoin;
  finJoin.kind = FrameKind::Finish;
  finJoin.finish = [this, join, thenB, elseB, thenSlot, elseSlot, op]() {
    // ★ 入边的"源块"必须是各分支**实际走到的出口块**（可能有内层 if 的汇合块），
    //   不是分支的入口块 —— 见 Frame::finishSlot 的说明。
    BasicBlock* tExit = exitBlockOf(thenSlot);
    BasicBlock* eExit = exitBlockOf(elseSlot);
    if (tExit == nullptr) tExit = thenB;
    if (eExit == nullptr) eExit = elseB;
    // 已经终结的块（`break`/`return`/`if` 的自分支跳转）**不能再补跳转**
    if (!hasTerm(tExit)) { cur_ = tExit; emit(createBr(join, op->loc)); }
    if (!hasTerm(eExit)) { cur_ = eExit; emit(createBr(join, op->loc)); }
    std::vector<Env> envs{exitEnvOf(thenSlot), exitEnvOf(elseSlot)};
    std::vector<BasicBlock*> edges{tExit, eExit};
    enterJoin(join, envs, op->loc, edges);
  };
  stack_.push_back(finJoin);
  stack_.push_back(felse);
  stack_.push_back(finElse);
  stack_.push_back(fthen);
  return Action::kOk;
}

// ============================================================================
// `(While) { cond } { body }` →
//     [pre] → [head: cond] --true--> [body] --→ [head]（回边）
//                         --false-> [exit]
//   ★ 循环头是**汇合点**（进入边 + 回边）⇒ 循环携带的变量在这里放 φ。
//   ★ 出口块也是汇合点（正常退出 + 每个 `break`）⇒ 出口处需要选择的值也放 φ。
// ============================================================================
Action FlatBuilder::lowerWhile(Op* op, Frame& /*fr*/) {
  Region* condR = op->region(0);
  Region* bodyR = op->region(1);
  const Env pre = env_;

  // 循环携带的候选 = 体内/条件里被写的槽 ∩ 被读过的槽 ∩ 进入边已有值的槽。
  //   （"进入边没有值"的槽说明循环外从未写过它 ⇒ 不需要 φ：每一轮都会被写。
  //     真出现"先读后写"就是源码里的 UB，零值兜底即可。）
  std::unordered_set<Value*> writes;
  collectStoredSlots(bodyR, writes);
  collectStoredSlots(condR, writes);
  std::vector<Value*> carry;
  for (Value* slot : writes) {
    if (slot == nullptr || !isReadSlot(slot)) continue;
    if (pre.find(slot) == pre.end()) continue;
    carry.push_back(slot);
  }
  // 集合的遍历顺序不稳定 ⇒ 按**值的创建编号**排序，保证同一输入两次展平
  //   产出逐字节相同的 IR（dump 是纯函数的一部分）。
  std::sort(carry.begin(), carry.end(),
            [](Value* a, Value* b) { return a->id() < b->id(); });

  BasicBlock* head = newBlock("loop.head");
  BasicBlock* body = newBlock("loop.body");
  BasicBlock* exit = newBlock("loop.exit");
  if (failed_) return Action::kStop;
  BasicBlock* preB = cur_;
  preB->addInst(createBr(head, op->loc));
  ownInst(preB->back());

  // ★ 立刻把"当前块"切到循环头（见上面那段说明：**必须在建 φ 之后、
  //   展开条件之前**）。第一版只在建完 φ 之后才切块，于是条件区的指令
  //   全落进了循环前块（`br i1` 也跟着落进去 ⇒ "br 后面还有指令"）。
  Env headEnv = pre;
  cur_ = head;
  env_ = headEnv;
  // ★ 回边的**源块**要等体走完才知道（体内若有嵌套 `if`，出口是它的汇合块）。
  //   先留一个槽位，收尾帧填。
  const int backedgeSlot = allocFinishSlot();
  std::vector<Instruction*> carryPhis;
  std::vector<Value*> carrySlots;
  for (Value* slot : carry) {
    Value* init = pre.at(slot);
    // 入值 = [进入边的值, 回边（先占位，体展开完后由收尾帧填真值）]
    Instruction* phi = createPhi(slotType(slot), {init, init}, {preB, body}, op->loc);
    ownInst(phi);
    head->addInst(phi);
    headEnv[slot] = phi;
    carryPhis.push_back(phi);
    carrySlots.push_back(slot);
  }
  env_ = headEnv;   // ★ 条件区要用**含 φ 的**环境（见下）
  // ★★ 把 `env_` 切到 `headEnv`（含刚建的 φ）**之后**才展开条件区 ★★
  //   否则条件里对"循环携带变量"的 `load` **不会**被替换成 φ，而是照原样
  //   引用循环前的那个 SSA 值 ⇒ 每轮用同一个旧值判断（语义错），
  //   而且 φ 与那个旧值形成"两个定义"的假象。
  //   实测（`25_while_if.sy` 的 `deepWhileBr`）：条件是 `%5 = load s`
  //   （循环前的那条），`%8 = φ` 造出来却没人用 ⇒ 条件里的 `%5` 永远不变。
  //   ⚠️ `enterBlock` 会把 `env_` 设成 `headEnv` 并把"当前块"设成 head，
  //      与下面的条件展开用的是同一个环境。

  const int condSlot = allocFinishSlot();
  const int bodySlot = allocFinishSlot();
  Frame fcond;
  fcond.region = condR;
  fcond.startBlock = head;
  fcond.cont = body;          // 条件为真 ⇒ 进体（`Yield <true>` 走这里）
  fcond.inLoop = true;
  fcond.loopExit = exit;
  fcond.loopHead = body;
  fcond.isWhileCond = true;
  fcond.pendingCarry = carryPhis;
  fcond.pendingCarrySlots = carrySlots;
  fcond.finishSlot = condSlot;

  Frame fbody;
  fbody.region = bodyR;
  fbody.startBlock = body;
  fbody.cont = head;          // 体走完 ⇒ 回边到循环头
  fbody.inLoop = true;
  fbody.loopExit = exit;
  fbody.loopHead = head;
  fbody.finishSlot = bodySlot;
  fbody.backedgeSlot = backedgeSlot;

  const size_t breaksMark = breaks_.size();
  Frame fin;
  fin.kind = FrameKind::Finish;
  fin.finish = [this, head, condSlot, bodySlot, backedgeSlot, exit, carryPhis, carrySlots,
                breaksMark, op]() {
    const Env& bodyEnv = exitEnvOf(bodySlot);
    BasicBlock* back = exitBlockOf(backedgeSlot);
    if (back == nullptr) back = head;
    // ① 回边入值：体出口环境里的值；该槽这一轮没被写 ⇒ 补零。
    //    同时把 φ 的"回边前驱块"改成**体真正的出口块**（见上面的说明）。
    for (size_t k = 0; k < carryPhis.size(); ++k) {
      const auto it = bodyEnv.find(carrySlots[k]);
      Value* v = (it != bodyEnv.end()) ? it->second : zeroOfSlot(carrySlots[k], op->loc);
      carryPhis[k]->setOperand(1, v);
      carryPhis[k]->setSucc(1, back);
    }
    // ② 出口块的 φ：入边 = 循环头（条件为假）+ 每个 break 的那一刻
    std::vector<Env> envs;
    std::vector<BasicBlock*> edges;
    envs.push_back(exitEnvOf(condSlot));
    edges.push_back(exitBlockOf(condSlot) != nullptr ? exitBlockOf(condSlot) : head);
    for (size_t k = breaksMark; k < breaks_.size(); ++k) {
      envs.push_back(breaks_[k].second);
      edges.push_back(breaks_[k].first);
    }
    enterJoin(exit, envs, op->loc, edges);
    breaks_.resize(breaksMark);
  };
  // 帧序：① 条件区（发 `br i1 …, body, exit`）
  //       ② **切到体块**（`headEnv` 是体的入口环境：循环携带变量已经换成 φ）
  //       ③ 体区（走完回边到 head）
  //       ④ 收尾（补回边入值 + 出口块的 φ）
  //   ⚠️ ② 这个"切换帧"是必需的：没有它，**体的指令会全落进循环头块**
  //      （实测症状：`br i1` 之后跟着一长串体指令，V4 报"终结符不是最后一条"）。
  Frame enterBody;
  enterBody.kind = FrameKind::Finish;
  enterBody.finish = [this, body, headEnv]() { enterBlock(body, headEnv); };
  stack_.push_back(fin);
  stack_.push_back(fbody);
  stack_.push_back(enterBody);
  stack_.push_back(fcond);
  return Action::kOk;
}

// ============================================================================
// `(For iv lo hi step) { body }`（S05b 规范化后的形状）→
//     [pre: br head] → [head: φ(iv)=lo, icmp slt] --T--> [body] → [inc] → [head]
//                                                  \--F--> [exit]
//   ★ `lo`/`hi`/`step` 在**循环前块**求值一次（约定 6.2.2）。
//   ★ IV 槽在出口处的值 = **首次不满足条件的那个值**（约定 6.2.1）：
//     `inc` 里先 `store iv.next` 再 `br head`，而出口时不经过 `inc`
//     ⇒ 槽里留的是最后一次 store 的值（条件为假那次没有再自增）。
// ============================================================================
Action FlatBuilder::lowerFor(Op* op, Frame& /*fr*/) {
  Value* ivSlot = map(op->operand(0));
  Value* lo = map(op->operand(1));
  Value* hi = map(op->operand(2));
  Value* st = map(op->operand(3));
  Region* bodyR = op->region(0);
  if (ivSlot == nullptr || lo == nullptr || hi == nullptr || st == nullptr) {
    giveUp("For 的操作数缺失", op->loc);
    return Action::kStop;
  }
  const Env pre = env_;
  BasicBlock* preB = cur_;
  BasicBlock* head = newBlock("for.head");
  BasicBlock* body = newBlock("for.body");
  BasicBlock* inc = newBlock("for.inc");    // 只有需要时才用得上（见上）
  BasicBlock* exit = newBlock("for.exit");
  if (failed_) return Action::kStop;
  // ⚠️ `br head` 在下面"前驱求值"之后再发（见那里的说明）：
  //   若 `lo` 解析成了 IV 的 φ，我们要在循环前块补一条 `load`，
  //   而那必须排在 `br head` 之前。**这里千万不要再发一次**（发了就是
  //   一个块里两条终结符 —— 实测被 V4 抓出来）。

  // ★ `iv = iv + step` 由谁做？（一个**必须想清楚**的点）
  //   结构化层 `ForOp` 的 `<step>` 是**属性**，体里不再有自增语句
  //   （S05b 把它摘掉了）。但**规范化前的 `while` 体里那句 `i = i + 1`
  //   可能还留着**（S05b 只摘它识别为步进的那一处；实测 `04-while-loop`
  //   的体里就**还有**一条 `i = i + 1`，于是第一版把它和新造的自增
  //   一起发了出来 —— 循环每轮 +2，多出来的那条还让"体出口的值"变成
  //   一个定义在体内的临时值，直接触发 V2 支配违规）。
  //   ⇒ 判据：**体在回边上是否已经更新了 IV 槽**。判据的输入就是体的
  //     出口环境（`bodyEnv.count(ivSlot)`）：体写了 IV 槽 ⇒ 复用它；
  //     没写 ⇒ 才新建 `for.inc` 块补 `iv.next = iv + step`。
  bool bodyWritesIv = false;
  {
    // 预扫体（含嵌套）：有没有 `store` 到 IV 槽
    std::unordered_set<Value*> w;
    collectStoredSlots(bodyR, w);
    bodyWritesIv = w.count(ivSlot) > 0;
  }

  std::unordered_set<Value*> writes;
  collectStoredSlots(bodyR, writes);
  writes.erase(ivSlot);
  std::vector<Value*> carry;
  for (Value* slot : writes) {
    if (slot == nullptr || !isReadSlot(slot)) continue;
    if (pre.find(slot) == pre.end()) continue;
    carry.push_back(slot);
  }
  std::sort(carry.begin(), carry.end(),
            [](Value* a, Value* b) { return a->id() < b->id(); });

  // ① IV 的 φ（入值 = lo；回边入值先占位，收尾帧填真值）
  Instruction* ivPhi = createPhi(typePool().i32(), {lo, lo},
                                 {preB, bodyWritesIv ? body : inc}, op->loc);
  ownInst(ivPhi);
  head->addInst(ivPhi);
  // ② 其它循环携带变量
  Env headEnv = pre;
  headEnv[ivSlot] = ivPhi;
  std::vector<Value*> carrySlots;
  std::vector<Instruction*> carryPhis;
  for (Value* slot : carry) {
    Value* init = pre.at(slot);
    Instruction* phi = createPhi(slotType(slot), {init, init},
                                 {preB, bodyWritesIv ? body : inc}, op->loc);
    ownInst(phi);
    head->addInst(phi);
    headEnv[slot] = phi;
    carrySlots.push_back(slot);
    carryPhis.push_back(phi);
  }
  // ★ `lo` / `hi` / `step` 里若有"其实解析成了 IV φ"的（S05b 的 `<lower>`
  //   就是条件里那条 `Load i`）⇒ 在**循环前块**重发一条 load
  //   （`preheaderValue` 的说明里有完整推导）。此刻 `cur_` 还是 preB。
  {
    const Type* ivTy = (ivSlot->type() != nullptr && ivSlot->type()->isPtr())
                           ? ivSlot->type()->elem
                           : typePool().i32();
    if (lo == ivPhi) lo = preheaderValue(op->operand(1), ivPhi, ivTy, preB, op->loc);
    if (hi == ivPhi) hi = preheaderValue(op->operand(2), ivPhi, ivTy, preB, op->loc);
    if (st == ivPhi) st = preheaderValue(op->operand(3), ivPhi, ivTy, preB, op->loc);
    ivPhi->setOperand(0, lo);
  }
  // 前驱块的终结符：现在才发（所以新 load 排在它前面）
  preB->addInst(createBr(head, op->loc));
  ownInst(preB->back());

  const int bodySlot = allocFinishSlot();
  const int backedgeSlot = allocFinishSlot();
  // ★ 循环头（= 出口边）的环境快照：`env_` 在体的展开过程中会被改写，
  //   所以这里复制一份留给出口块的 φ。**不能用体出口的环境**：
  //   那里面可能有"定义在体内"的临时值，用它当出口块的入值会直接
  //   违反 SSA 支配（实测被 V2 抓出来：`%19` 定义在 L2、用在 L4）。
  const Env headExitEnv = headEnv;

  // ③ 条件：`icmp slt iv, hi` 直接发在 head 里（hi 定义在 preB，支配 head）
  cur_ = head;
  env_ = headEnv;
  Instruction* cond = createICmp(IPred::Slt, ivPhi, hi, op->loc);
  ownInst(cond);
  head->addInst(cond);
  head->addInst(createCondBr(cond, body, exit, op->loc));
  ownInst(head->back());

  // ④ 体 → inc
  enterBlock(body, headEnv);
  Frame fbody;
  fbody.region = bodyR;
  fbody.startBlock = body;
  fbody.cont = bodyWritesIv ? head : inc;   // 体自己更新 IV ⇒ 直接回边
  fbody.inLoop = true;
  fbody.loopExit = exit;
  fbody.loopHead = head;
  fbody.finishSlot = bodySlot;
  fbody.backedgeSlot = backedgeSlot;

  const size_t breaksMark = breaks_.size();
  Frame fin;
  fin.kind = FrameKind::Finish;
  fin.finish = [this, head, body, inc, exit, ivPhi, ivSlot, st, carryPhis, carrySlots,
                bodySlot, backedgeSlot, headExitEnv, bodyWritesIv, breaksMark, op]() {
    // ① 回边的 IV 更新：体自己做过就**不重复做**（见上面的长注释）
    const Env& bodyEnv = exitEnvOf(bodySlot);
    env_ = bodyEnv;
    // ★ "体是否更新了 IV"以**体出口环境**为准（不是预扫的标志）：预扫用的是
    //   `collectStoredSlots`（含嵌套分支），而分支里的 store **未必**在每条
    //   路径上都执行 —— 实测 `fft0.sy` 的 `multiply`：预扫说"体写了 IV"，
    //   于是我们既不建自增块、又让 φ 的回边指向自增块 ⇒ 回边指向一个
    //   **没有终结符的块** ⇒ 前驱表里没有它 ⇒ "φ 2 个入值 vs 1 个前驱"。
    //   ⇒ 判据改成"体出口环境里有没有 IV 槽"（那才是真的走到过的写），
    //     并把 φ 的回边前驱块一起改成体出口块。
    //   判据用**预扫标志**（体里有没有对 IV 槽的写）与**出口环境**的**与**：
    //   只要体里没有写 IV，就必须由自增块补；写了就复用它。
    const bool bodyUpdatedIv = bodyWritesIv && (bodyEnv.find(ivSlot) != bodyEnv.end());
    if (bodyUpdatedIv) {
      BasicBlock* be = exitBlockOf(backedgeSlot);
      ivPhi->setSucc(1, be != nullptr ? be : body);
    }
    if (!bodyUpdatedIv) {
      // 体没有更新 IV ⇒ 新建自增块：`iv.next = add iv, step` → `store` → `br head`
      cur_ = inc;
      Instruction* next = createBin(Opcode::Add, ivPhi, st, op->loc);
      ownInst(next);
      inc->addInst(next);
      inc->addInst(createStore(typePool().i32(), next, ivSlot, op->loc));
      ownInst(inc->back());
      inc->addInst(createBr(head, op->loc));
      ownInst(inc->back());
      ivPhi->setOperand(1, next);
      env_[ivSlot] = next;
    } else {
      // 体更新过：回边入值 = 体出口环境里 IV 槽的值（体里那条 `store` 的值）
      const auto it = bodyEnv.find(ivSlot);
      ivPhi->setOperand(1, it != bodyEnv.end() ? it->second : ivPhi);
    }
    // ② 循环携带变量的回边入值
    for (size_t k = 0; k < carryPhis.size(); ++k) {
      const auto it = bodyEnv.find(carrySlots[k]);
      Value* v = (it != bodyEnv.end()) ? it->second : zeroOfSlot(carrySlots[k], op->loc);
      carryPhis[k]->setOperand(1, v);
    }
    // ③ 出口块的 φ：**循环头那一刻的环境**（不是体出口 —— 见上面的说明）
    //    + 每个 break
    std::vector<Env> envs;
    std::vector<BasicBlock*> edges;
    envs.push_back(headExitEnv);
    edges.push_back(head);
    for (size_t k = breaksMark; k < breaks_.size(); ++k) {
      envs.push_back(breaks_[k].second);
      edges.push_back(breaks_[k].first);
    }
    enterJoin(exit, envs, op->loc, edges);
    breaks_.resize(breaksMark);
  };
  stack_.push_back(fin);
  stack_.push_back(fbody);
  return Action::kOk;
}

}  // namespace flat
}  // namespace sysy
