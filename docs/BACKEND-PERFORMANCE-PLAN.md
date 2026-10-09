# 自研后端与性能决策总规划

> 状态：规划权威入口。本文描述尚未实现的正式后端、目标性能模型和中后端反馈路线；
> `CURRENT-STATE.md` 仍是“已经实现了什么”的唯一当前状态入口。
>
> 产品目标只有一个：在 SysY2022 语义正确、ABI/比赛规则和资源限制全部满足的前提下，
> 让自研编译器在 **ARMv8-A Cortex-A53** 与 **RV64GC / BOOM v3** 上生成的最终机器代码
> **真实运行时间尽可能短**。IR 漂亮、pass 数量、静态指令数和架构完整度都只是手段或代理指标。

## 1. 决策优先级

1. 正确性、标准 ABI、比赛合规是硬门禁；不以性能交换错误结果。
2. 硬门禁后，以目标机真实 runtime 为首要结果；同时记录 worst regression，而不是只看 aggregate。
3. 编译时间、峰值 RSS、二进制/代码体积是显式成本，但除非触及预算，不与 runtime 等权。
4. 架构取舍优先 Codex 可读性和职责边界；抽象必须有真实 consumer。
5. 不因“上游理论上应该做过”而拒绝 MIR 层仍然存在且可盈利的安全优化。
6. 也不因 LLVM/其他编译器存在某 pass 就默认需要；以当前 workload、两个目标和实测收益决定投资。
7. 所有性能 pass 必须可关闭、可 dump、可统计、可 A/B；复杂搜索必须有预算和安全回退。
8. Legality 与 Profitability 分离：先证明能做，再比较是否值得做。

## 2. 总体后端流水线

正式后端优先消费内存中的 `flat::Module`；`.ll` 保留为稳定协议、oracle 和独立验证入口。
S11b 的 `.ll -> x86` toy backend 仍只承担契约验证，不演化成正式性能后端。

```text
flat::Module (SSA CFG)
        |
        +--------------------------> LLVMTextEmitter (oracle/debug protocol)
        |
        v
BackendLowering / ABI Call Lowering
        |
        v
Target MIR, SSA, virtual registers
        |
        v
Legalize -> local costed Pattern ISel -> MachineCombine
        |
        v
MachineCSE / cheap pre-RA cleanup
        |
        v
Pre-RA Machine Scheduler
        |
        v
========== Frozen Pre-RA MIR ==========
        |
        v
Register Allocation
  liveness / interference / affinity / spill / rematerialize
        |
        v
PHI -> PARCOPY -> parallel-copy resolution
        |
        v
Post-RA canonicalization
  copy propagation / reload forwarding / dead instruction cleanup
        |
        v
FrameLayout -> Prologue/Epilogue -> FrameIndex elimination
        |
        v
Target Post-RA Optimize
        |
        v
LateLegalize / pseudo expansion
        |
        v
Post-RA Scheduler
        |
        v
MachineBlockPlacement -> BranchOptimization -> BranchRelaxation
        |
        v
FinalCleanup (近似只删不增)
        |
        v
GAS AsmPrinter
```

不要等所有中端优化完成再开始正式后端。最低后端、MIR、ABI、RA 和实验基础应与中端分析/优化共同演进，
否则中端 cost model 会长期对真实机器成本失明。

### 2.1 Flat mutation / Pass / Analysis 管理边界

不要复制 mini-LLVM，但也不能让正式 analyses/backend 继续建立在“裸改 operand/CFG，最后全量 `rebuildUseDef/rebuildCFG`”的第一版容器模型上。S11b 后、S12 前执行 `SESSION-PLAN.md` 的 F00–F05 foundation：

1. F00：完整 `Value -> Use{Instruction*, operandIndex}`，Argument 独立 Value，canonical Constant，稳定文本协议与内存模型解耦；
2. F01：`IRMutator + CFGEditor`、phi incoming 原子维护、owned/live instruction 分离、MutationSummary/IRRevision；
3. F02：revision-aware、dependency-aware、fail-closed 的 AnalysisManager；
4. F03：FlatPassManager/PassContext/instrumentation/verify-each/ablation；
5. F04：S09 Mem2Reg 迁移为第一个正式 FunctionPass；
6. F05：CompilationSession 与 CMake component boundary 收敛。

`PreservedAnalyses` 只是 pass 的声明，真实 mutation/revision 是事实。二者冲突时必须强制 invalidate；第一版采用 invalidate → lazy recompute，宁可多失效，不允许 stale analysis。`changed` 也不由 pass 自报，而应从 mutation session 自动推导。PassResult 保留语义状态和统计，例如：

```cpp
enum class PassStatus { NoChange, Applied, Declined, Failed };

struct PassResult {
    PassStatus status;
    PreservedAnalyses preserved;
    PassStats stats;
};
```

AnalysisManager 从真实 consumer 开始逐步拥有 CallGraph/FunctionSummary、DominatorTree/LoopInfo/Alias/SCEV/ValueFacts/BPI/BFI/Dependence。Module/Function scope 先落地；CGSCC/Loop scope 在 S12/S14 出现首个 consumer 时再增加，不预造万能 adaptor。只有 DSE/load elimination 等真实 consumer 证明需要时，才增加轻量 `MemoryDefUse`/MemorySSA-like 结构。

Structured pass execution 与 Flat pass execution 分开管理，`FlattenCFG` 永远是显式 type-changing boundary。Backend/Machine pipeline 同样声明每阶段 input/output、preserved/invalidated analysis、预算和 failure atomicity。B00 的只读 target/ABI audit 可与 foundation 交错；B01 MIR 设计也可提前，但正式 Flat→Backend integration 不得绕过 F01 mutation/live 契约。

复杂 transform 后允许明确 cleanup group（例如 SimplifyCFG/DCE/CSE），但 cleanup ownership 必须显式，不能靠“某个后续 pass 恰好会修”。


## 3. MIR：第一天就必须具备的表达能力

### 3.1 指令和操作数

采用机器指令式表示，不延续“Instruction 自己就是唯一 SSA Value”的前中端模型：

```cpp
struct MachineInstr {
    Opcode opcode;
    SmallVector<MachineOperand> operands;
};
```

`MachineOperand` 至少支持：

- virtual register；
- physical register；
- immediate；
- `FrameIndex`；
- global/external symbol；
- `MachineBasicBlock`；
- `RegMask`；
- explicit / implicit def-use 标志、tied operand 约束。

一条指令允许 0..N 个 def。这样可以自然表示 pair load、post-index、two-address/destructive form、固定寄存器约束，
不需要因单结果表示制造隐藏 pass-order 契约。

### 3.2 Physical register、register unit 与 class

物理寄存器身份表示硬件 storage，而不是把 `x0/w0`、`v0/d0/s0` 当互不相关的寄存器。
TargetRegisterInfo 提供 alias/register-unit 关系和 view/operand class。

至少区分：

- register bank：GPR 与 FP/SIMD；
- register class：AArch64 `GPR32/GPR64/FPR32/FPR64/SIMD128`，RV64 对应整数/浮点 class；
- reserved、caller-saved、callee-saved、argument/return registers；
- allocatable order/candidates，但最终分配成本不等于固定数组顺序。

AArch64 从第一天保留 `SIMD128` 的表达能力，即使第一版不做向量 RA。

### 3.3 MachineMemOperand

每个 load/store 保留独立的机器内存描述：

```cpp
struct MachineMemOperand {
    AccessKind kind;       // load/store
    uint32_t size;
    uint32_t alignment;
    MemoryObjectId object; // 可为 unknown
    optional<int64_t> knownOffset;
};
```

Lowering 尽量把 flat IR 的 base/provenance/noalias 事实传入 MIR。Scheduler、load CSE、store-load forwarding
只能在证明安全时跨越内存操作；不要在降级时把所有内存访问退化成“未知地址”。

### 3.4 FrameIndex

local alloca、spill、callee-save 和 outgoing arg 全部先成为 abstract frame object；RA 完成后才分配最终 offset。
禁止在 BackendLowering 阶段把 alloca 直接固化成 `sp + constant`。

### 3.5 MIR verifier / dump / parser

至少有两个 verifier 阶段：

- pre-RA：SSA、def-use、register class、operand constraint、PHI/CFG、call regmask、FrameIndex 合法；
- post-RA：无未分配 vreg、physical register alias/def-use、frame/branch/pseudo 状态符合阶段约束。

MIR 必须可稳定 dump/hash/replay。RA Lab 与 Scheduler Lab 依赖它冻结实验输入。

## 4. Target abstraction：只保留真实需要的七件事

```text
TargetMachine
  |- TargetRegisterInfo
  |- TargetInstrInfo
  |- TargetABIInfo
  |- TargetLowering
  |- TargetFrameLowering
  |- TargetSchedModel
  `- TargetAsmPrinter
```

不要复制 LLVM 的 TargetRegistry/TableGen/MC/object-writer 体系。这里只有两个目标，最终输出 GAS `.s`。
AsmPrinter 不做隐藏 lowering：所有 pseudo expansion、large immediate、frame offset 和 long branch 处理必须先在 MIR 可见。

## 5. ABI、Call 与 Frame

### 5.1 标准 ABI 是 V1 唯一调用约定

所有内部和外部函数先统一使用平台标准 C ABI。InternalFastCC 只有在标准 ABI 的残余开销被 profile 证明重要后再评估，
且仅允许 closed-world、internal、non-address-taken 函数。

RISC-V `-march=rv64gc` 本身不确定 ABI 名称。正式后端冻结前必须用比赛 GCC/runtime 做互调 probe，确认实际 ABI；
预期硬浮点环境时按 LP64D 规则设计，但不能把“预期”当工具链证据。

### 5.2 Call Lowering 在 RA 前完成

普通调用在 MIR 中显式形成固定寄存器 copy、stack arg frame object、`CALL @sym, regmask` 和返回寄存器 copy。
RA 通过固定 physical operands 与 `RegMask` 看见 ABI clobber，不使用伪造 placeholder 模拟 caller-saved 冲突。

argument/return/call-copy 同时进入 affinity graph；热 callsite 的 affinity 权重由 BFI 提供，使 RA 有机会直接把值分到 ABI 寄存器并消掉 copy。

### 5.3 Reserved outgoing call frame

SysY 无动态 alloca，direct call 参数数量可静态得知。V1 默认一次性预留本函数最大 outgoing stack argument area，
正常函数主体 SP 保持不变：

```text
frame = locals + spills + callee saves + max outgoing args + alignment
```

只有实际数据证明巨大递归 frame footprint 成为问题时，才考虑对超大 outgoing area 使用其他策略。

### 5.4 FrameLayout

`MachineFrameInfo` 至少区分 `Local/Spill/CalleeSave/OutgoingArg`，记录 size/alignment，未来可附 weighted access count。
布局不仅追求正确，还要服务目标寻址：

- 热 spill slot 尽量放在便宜 offset 范围；
- AArch64 callee-save slot 采用 pair-friendly 布局；
- 大而冷的 local 可放远；
- outgoing area 保持 ABI 对齐。

默认能省 frame pointer 就省，但是否允许 FP 寄存器参与 RA 由目标/平台 policy 决定。

### 5.5 Leaf、frameless、tail call 与 shrink wrapping

`isLeaf`、`needsStackFrame`、`needsReturnAddressSave` 分开建模。只有 tail call 的函数不应因为 `hasCall=true` 就无谓保存 LR/RA。

- tail recursion transformation 尽量在中端做；
- sibling/general tail call 最终 legality 由 backend 根据 ABI/frame 判断，并在 MIR 中显式使用 `TAILCALL`；
- 多 return 的 shared/duplicated epilogue 用 BFI 与 epilogue size 决定；
- shrink wrapping 列入性能阶段实验，不进入最小 bring-up。

RA cost 必须逐步加入“首次启用一个 callee-saved register”的 save/restore 成本，以及 live range 跨热 call 的成本。

## 6. Instruction Selection

### 6.1 选择方案

不做完整 SelectionDAG，也不把“机械一对一 lowering + fixed-point peephole”作为最终形态。
采用：

```text
TargetLegalizer
    -> bounded local use-def tree/DAG matching
    -> target legality
    -> costed candidate selection
    -> Target MIR
    -> MachineCombine
```

V1 匹配限制在同 basic block、纯 use-def 链、小深度/小节点预算。默认只吞 single-use producer；multi-use duplication 必须由 cost 明确批准。

### 6.2 重要 pattern

AArch64 优先：

- base + immediate / base + scaled index / extend+shift addressing；
- shift/extend folded arithmetic；
- integer `mul+add/sub -> MADD/MSUB`；
- compare + branch/select；
- 合法常量和 symbol materialization；
- 后续 SIMD/NEON patterns。

RV64GC 优先：

- `ADD/ADDW + immediate -> ADDI/ADDIW`；
- compare + branch 直接形成 `BEQ/BNE/BLT/...`；
- `W` 指令维持 SysY i32 canonical sign-extended representation；
- large immediate/address sequence；
- base+imm load/store 与 pointer induction 配合。

浮点 fusion/reassociation 永远先受严格 FP legality 限制；不能因为 FMA 更快就改变运算顺序。

## 7. Register Allocation

### 7.1 正式 V1 方向

纯 linear scan 适合作 baseline，但正式性能 V1 优先：

```text
SSA MIR
 -> Liveness
 -> Interference Graph
 -> Affinity Graph
 -> precolored/fixed constraints
 -> dynamic/priority greedy coloring
 -> spill + rematerialization
 -> spill-slot coloring
 -> PHI to PARCOPY
 -> parallel-copy resolution
```

Affinity 明确建图，不使用 `prefer[value]=phi` 单指针：

```cpp
struct AffinityEdge { VReg a; VReg b; double weight; };
```

来源包括 PHI/COPY/argument/return/call fixed-reg affinity，权重使用 BFI/loop hotness。

### 7.2 Coloring 与 spill cost

第一版可以用 DSATUR-like 动态选择：优先固定/高 saturation、高 spill weight、高 degree，并保持 deterministic tie break。
spill weight 先使用 BFI；BFI 未就绪时退化到 loop-depth weight。Rematerializable value（小 immediate、global address、可重建 frame address）显著折扣 spill cost。

若已经有 interference graph，spill-slot coloring 是低成本高收益项。跨 register bank spill、预留少量 spill physical register 等策略必须是 target-specific 可关闭实验，不写成 allocator 核心不变量。

### 7.3 RA Lab

详细协议见 `RA-LAB.md`。正式 allocator 从接口开始可替换：

```cpp
AllocationResult run(MachineFunction&, const TargetRegisterInfo&, const AllocationOptions&);
```

支持至少 `linear`、简单 greedy、SSA greedy/DSATUR-like。LLVM allocator 只进入开发实验室，不进入比赛编译器依赖。

## 8. Machine Scheduler

共享 `ScheduleDAG`，target 只提供 `TargetSchedModel` 与小量 policy 参数，不复制两套 scheduler。

### 8.1 Pre-RA

只做 basic-block scheduling。目标是 latency/ILP 与 register pressure 的混合：

```text
Comfortable pressure -> latency / critical path first
NearLimit            -> balance
Overflow             -> pressure first
```

pressure penalty 非线性，不能用 `pressure * constant`。call/volatile/未知强副作用 V1 可作为 region boundary。
内存依赖使用 `MachineMemOperand` 和 alias facts。

### 8.2 Post-RA

不再优化 vreg pressure；关注真实 physical dependency、load-use latency、resource conflict 和 issue pairing。
V1 不做 post-RA anti-dependency renaming；若硬件计数证明值得，再独立实现 `PostRARenamer`。

A53 是主要 scheduling 优化目标；BOOM 的 OoO 能缓冲部分坏顺序，但仍受有限 issue window、资源和依赖链影响。

每次调度可选输出 decision trace：ready candidates、被选节点、critical-path/pressure/resource 原因。

## 9. BPI/BFI、Block Placement 与 Branch

增加共享分析：

```text
BranchProbabilityInfo
BlockFrequencyInfo
```

信息源优先级：真实 profile（未来） > 已知/估计 trip count > loop structure > 少量通用静态 heuristic > unknown 50/50。
不要堆大量不可解释 branch magic numbers。

BFI 同时服务：RA spill/affinity、scheduler、ISel profitability、inline/unroll、frame hotness 和 block placement。

MachineBlockPlacement 先保持 inner-loop chain 连续，再按热 edge 合并 chain，目标最大化 weighted fallthrough。
随后独立运行 BranchOptimization（invert、fallthrough jump deletion、jump-only bypass）与 BranchRelaxation。
RV64 conditional branch range 必须有保守 relaxation；MIR 阶段不要假装知道 assembler/linker 最终压缩/relax 后精确长度。

V1 不做 tail duplication；以后只对 tiny/hot block 作为可关闭实验。

## 10. Machine optimization

### 10.1 Pre-RA

MachineCSE 是正式优化，不因“上游应该已经做过”而拒绝：

- V1：basic-block pure CSE；
- 很快升级到 dominator-tree CSE；
- commutative operands canonicalize；
- 普通 load 不混入纯 CSE，单独发展有内存证明的 MachineLoadCSE。

MachineCSE 统计命中 opcode 与产生阶段。如果冗余大量来自某 Lowering/ISel，应修上游，同时保留 CSE 作为生产防线。

MachineLICM、sinking 等不永久排除：只有 target lowering 后新产生、实测命中且盈利时加入。

### 10.2 Post-RA

高优先级：

- copy propagation / self-copy removal；
- known spill-slot reload forwarding；
- dead physical-def cleanup；
- proven store->load forwarding；
- AArch64 LDP/STP formation；
- target-specific physical-register combine。

主 Post-RA Optimize 必须在最终 Post-RA Scheduler 之前。Scheduler 后只留近似“deletion-only”的 FinalCleanup，避免重新制造未调度序列。

LateLegalizer 只负责把 remaining pseudo/illegal encoding 变成可编码机器序列，不负责 profitability。

## 11. Target-specific performance

### 11.1 AArch64 / Cortex-A53

优先级：

1. addressing mode / immediate / symbol materialization；
2. scalar fused patterns；
3. RA + A53 scheduler；
4. LDP/STP、frame pairing、block layout；
5. NEON fixed-width vectorization。

NEON vectorization 的循环规划应在拥有 LoopInfo/SCEV/dependence/alias 的中端做，MIR/ISel 负责把显式 fixed-width vector op 高质量降到 NEON。
先实现 map-like `v4i32/v4f32` loop；严格 FP reduction/reassociation/FMA contraction 必须另行证明，不因向量化而放松语义。

### 11.2 RV64GC / BOOM v3

不假设 V/Zba/Zbb/Zfa 等未写入比赛 target contract 的扩展。优化重点：

1. `W`-op canonicalization，减少无谓 sext/trunc；
2. pointer induction 与 base+imm addressing；
3. compare+branch、immediate/address materialization；
4. RA/ILP/scheduler；
5. C-extension-aware code-size tie-break。

C-extension 可压缩寄存器仅作为 secondary/tie-break 成本，不能为了压缩率制造 spill 或明显延长关键路径。

## 12. Target Performance Model

不要提供 `int cost(Instruction*)`。正式接口分成：

```cpp
struct TargetPerformanceInfo {
    TargetCapabilityInfo capability; // architecture fact / legality
    TargetCostModel cost;            // analytical estimate
    TargetSchedModel sched;          // latency/resource/uops
    TargetRegisterInfo regs;         // banks/classes/pressure budget
    TargetMemoryModel memory;        // cache/locality model
};
```

Architectural fact、microarchitectural model、heuristic policy 分开存放；例如“RV load offset 可编码范围”不是可调 heuristic，
“A53 load latency”属于 CPU model，“unroll pressure margin”才是 heuristic。

### 12.1 CostVector

```cpp
struct CostVector {
    double staticInsts;
    double uOps;
    double codeBytes;
    double reciprocalThroughput;
    double criticalPath;
    double loads, stores;
    double loadBytes, storeBytes;
    double branches;
    PressureEstimate pressure; // GPR/FPR/SIMD 分开
};
```

不做一个全局加权总分。InliningPolicy、UnrollPolicy、VectorPolicy、AddressPlanPolicy 根据自己的问题解释 CostVector。
query 必须带 use/context/hotness/trip count/value facts 等上下文，不能把“常量成本”脱离它能否折进 consumer 来估。

Estimate 还应带 `Exact/High/Medium/Low` confidence。明显 winner 直接决定；重要且不确定的候选升级到 probe。

### 12.2 MachineCostProbe

两级：

- L1 Pre-RA Probe：clone candidate region，跑 legalize/ISel/combine/CSE，得到真实 machine shape、critical path 和估计 pressure；
- L2 Full-Function Probe：只对最热、最模糊候选 clone 整个函数并跑到真实 RA 后，得到实际 spill/callee-save/frame/post-RA metrics。

二者都有严格 per-function/per-pass budget、PlanHash cache 和原 IR 安全回退。比赛编译期间绝不运行外部 benchmark 或在线学习。

### 12.3 Plan-first transformation

Inline/Unroll/Vector/Tile/Address 等复杂优化先建立不可变 plan：Analyze -> Enumerate -> Legalize -> Cost/Probe -> Select -> atomic Commit。
失败不留下半修改 IR。

## 13. 三个代表性决策推演

这三个案例作为 cost model/plan system 的设计测试。实现任何相关 policy 时必须能解释这些选择，而不是只返回一个 magic score。

### 13.1 整数 matmul：interchange -> tile -> vector/unroll/register block

假设 row-major：`C[i][j] += A[i][k] * B[k][j]`，且 dependence/alias 证明允许重排。

先枚举 loop order，而不是固定先 unroll。原始 `i-j-k` 对 B 的 k 方向通常是大 stride；候选 `i-k-j` 让 B 与 C 的 j 方向连续，A[i][k] 可在内层复用。
MemoryModel 先比较 line traffic/working set，淘汰明显差的顺序和 tile。

AArch64 候选：

```text
order i-k-j
+ cache tile (候选尺寸由 L1 model 过滤)
+ j 方向 VF=4 NEON
+ broadcast A[i][k]
+ vector B[k][j..j+3] / C[i][j..j+3]
+ UF=1/2/... 与 SIMD pressure 联合选择
```

32-bit integer wraparound语义允许使用非饱和的 32-bit vector mul/add，只要不改变源程序要求的其他顺序；不要把这条结论推广到 strict-FP reduction。
VF 与 UF 联合枚举，不能 vectorizer 选 VF 后让 unroller 独立把寄存器压力推爆。

RV64GC 无 vector 候选：优先同样改善 locality 的 loop order/tile，再对 j 内层使用 pointer induction、scalar unroll 和多个 C accumulator register block。
UF 候选必须看真实 GPR pressure；超过 comfortable budget 的收益迅速降权。

对 hottest matmul 可用 L2 Full-Function Probe 比较少量最终 plan，采集 actual spills、frame、machine inst 和 memory ops；最终参数由真实 A53/BOOM runtime 校准。

### 13.2 普通数组循环：AArch64 与 RV64 的 AddressPlan 应不同

例如：

```c
for (int i = 0; i < n; ++i)
    out[i] = a[i] + b[i];
```

计划 A：保留 index induction；计划 B：把 `a/b/out` 都改为 pointer induction。

AArch64 的 AddressMatcher 可把 `base + sext(i) << 2` 吞进多种 load/store addressing form，因此保留一个共享 index 可能比每轮更新三个 pointer 更便宜。
不能因为 IR 中出现 `i*4` 就机械做 strength reduction。

RV64GC 普通 load/store 依赖 base+small immediate，没有等价的 scaled-index load。若每次用 index 需要 shift/add，三个 pointer induction 往往能把主体收敛到 load/store + pointer `ADDI`。
所以 StrengthReduce 应构造 `AddressPlan` 并查询 target，而不是 ARM/RV 复制两套 pass。

L1 MachineCostProbe 直接比较两种 plan 的 machine inst、address materialization、critical path 和 pressure；不需要等完整 RA 才解决绝大多数此类选择。

### 13.3 Hot callsite：no-inline / inline / tail-call

对一个高 BFI callsite 同时评估：

- no-inline：保留 code size/pressure，但支付 call/return、ABI copies、callee frame；RA affinity 可能消掉部分 copy；
- inline：消掉调用边界并暴露 constants/CSE/LICM/branch folding，但扩大 live range、pressure 和 I-cache footprint；
- tail-call：只有语义与 ABI/frame 合法时才候选，不能暴露 callee body，但可省当前函数返回链/frame 开销。

静态模型先计算 call frequency、callee machine-size estimate、expected pressure delta、exposed constant facts。
冷 callsite 即使 callee 很小也可能保持 no-inline；inner-loop tiny callsite 更可能 inline。
若 hottest callsite 的 inline/no-inline 接近 pressure cliff，升级到 Full-Function Probe，用真实 RA spill/callee-save/frame 决定。

若调用结果被立即原样 return，先检查 tail-call legality；tailcall-only function 随后重新计算 `needsReturnAddressSave`。

## 14. 实验、校准与可视化

性能开发统一记录：source/input hash、compiler commit、target/cpu/features/ABI、pre-RA MIR hash（适用时）、toolchain、raw runtime samples、warmup、median/spread、
correctness、code bytes、machine instructions、cycles/instructions/loads/stores/branch/cache counters（可得时）。

真实 ELF/objdump 是 code size/真实指令数的最终来源；`.s` 中 pseudo 数量不是最终真值。

Cost model 记录 predicted vs actual backend metrics；dashboard 按 prediction error 排序，优先修系统性误差，而不是随机调阈值。
微 benchmark calibration 分别测 dependency latency、independent throughput、load-use、branch、FP、NEON，并明确目标硬件/BOOM 配置。
学习和调参离线进行；比赛编译器不在线跑 benchmark。

## 15. 开发里程碑（B 系列，接入现有 S00–S28 主线）

B 系列与 S 系列编号独立只是为了职责和历史稳定性，执行流程并不独立。`docs/handoff/SESSION-PLAN.md` 是统一调度权威：S11b 后先进入 F00–F05 foundation；B00 的只读 target/ABI/benchmark audit 可在 foundation 期间穿插，B01 的 MIR 设计可提前，但 live Flat→Backend integration 要等 F01 mutation/live 契约。F00–F05 完整验收后，B00–B04 与 S12–S14b 共同推进；B05–B09 与 S15–S16b 建立 cost/analysis/RA 基础；B10–B15 为 S17–S20 提供真实机器反馈；B16–B18 与 S21–S25 共同决定 target-aware loop plans；B19/B20 与 S26 一起完成最终调优。S27/S28 冻结和交付的是完整自研编译器，而不是仍待实现的后端。

| 阶段 | 目标 | 关键验收 |
|---|---|---|
| B00 | target/ABI/benchmark contract audit | 官方 ABI + 实际 toolchain probe；冻结 target flags、runner、raw sample schema |
| B01 | MIR core/verifier/dump/replay | multi-def、reg unit/class、FrameIndex、RegMask、MachineMemOperand；round-trip/hash 稳定 |
| B02 | RV64GC minimum correctness backend | Flat -> RV MIR -> all-spill/simple RA -> GAS，支持 manifest correctness |
| B03 | AArch64 minimum parity | 同一 MIR 抽象支撑 A64，验证 alias/register class/frame 差异 |
| B04 | formal ABI/Call/Frame | fixed-reg call lowering、reserved call frame、FrameIndex、standard ABI probes |
| B05 | Legalize + costed Pattern ISel | direct baseline 与 pattern selector A/B；关键 addressing/fused pattern |
| B06 | Machine analyses | MachineLoopInfo、BPI/BFI、liveness/pressure、mem provenance plumbing |
| B07 | Pre-RA scheduler | identity/latency/pressure/hybrid 可切换；A53/BOOM sched model |
| B08 | RA Lab + allocator baselines | frozen pre-RA MIR；linear/simple greedy；统一 metrics/dashboard |
| B09 | SSA performance RA | interference+affinity、dynamic greedy、remat、spill-slot coloring、PARCOPY |
| B10 | Frame/Post-RA foundation | callee-save cost、copy/reload cleanup、frame offset legality、leaf/frameless/tailcall |
| B11 | Machine optimization | dominator MachineCSE、store/load forwarding、A64 pair formation、late legalize |
| B12 | final scheduling/layout | post-RA scheduler、hot-chain placement、branch optimize/relax |
| B13 | AArch64 scalar tuning | addressing/materialization/fusion/RA/A53 schedule ablation |
| B14 | RV64GC scalar tuning | W-op、pointer induction、C-extension tie-break、BOOM schedule ablation |
| B15 | TargetPerformanceInfo 完整化 | S15 已冻结共享 CostVector/CostContext 抽象；本阶段补齐 A53/BOOM target facts、confidence、pressure/memory model、bounded probe、remarks 与 prediction metrics，不另造第二套 cost API |
| B16 | target-aware middle-end feedback | inline/unroll/address-strength-reduce 使用 BFI/cost/probe，不复制 target-specific pass |
| B17 | AArch64 NEON V1 | dependence合法的 fixed-width map loops；VF×UF 联合选择；strict-FP guards |
| B18 | cache transforms | matmul/conv/stencil 的 interchange/tiling/register block 使用 MemoryModel + probe |
| B19 | advanced measured opportunities | MachineLoadCSE、MachineLICM、shrink-wrap、tiny tail duplication 等只按证据进入 |
| B20 | final tuning/closeout | per-target geomean + per-case regression、hardware counters、cost calibration、handoff |

实现顺序可按真实 blocker调整，但 B01/B04/B08/B15 这四个边界不能靠 ad-hoc 代码绕过。

## 16. 当前还需要外部确认的 blocker

1. 比赛 RISC-V GCC/runtime 的实际 ABI（预计 LP64D，但必须 probe）；
2. Cortex-A53 与 BOOM v3 是否能稳定重复跑性能用例、可用哪些硬件 counters；
3. BOOM v3 的实际配置参数，不能用“通用 BOOM”网上数字硬编码 scheduler；
4. 比赛最终允许的 AArch64 ISA/NEON 范围与 assembler/linker 版本。

这些 blocker 不阻止 MIR/实验框架建设，但在对应 target cost/scheduler/ABI 冻结前必须解决。
