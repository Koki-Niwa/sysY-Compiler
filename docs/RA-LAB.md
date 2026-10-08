# RA / Scheduler / Codegen 实验室

> 目的：建立“其他变量不变，只替换一个后端策略”的可重复实验环境，并把 LLVM 18 的寄存器分配实现纳入对照。
> 这是开发工具与研究基础设施，不是比赛编译器依赖；比赛编译器仍禁止调用或链接 LLVM。

## 1. Controlled experiment 的定义

自研后端 RA 对比必须满足：

```text
same source/input
same target/cpu/features/ABI
same lowering/ISel/pre-RA scheduler
same Frozen Pre-RA MIR hash
ONLY allocator changes
same frame/post-RA/scheduler/layout/assembler/linker
same runtime machine/protocol
```

runner 机器检查 `pre_ra_mir_hash`、target config 和 post-RA pipeline config；任何不一致只能标为 `cross-pipeline comparison`，
禁止在 dashboard 上称作“RA speedup”。

Scheduler 实验类似，但 freeze 点不同：

- Pre-RA scheduler：冻结 pre-scheduler MIR，允许其改变 live range 和下游 RA；RA 变化是 scheduler 的因果结果，不是混杂变量。
- Post-RA scheduler：冻结 allocated MIR，物理分配完全相同，只比较指令顺序。

## 2. 两种 RA 问题必须分开

### RA-Core

保持 liveness/interference/spiller/rematerializer/copy resolver 相同，只替换 coloring/eviction/node-selection。
回答“核心分配算法谁更聪明”。

### RA-System

冻结相同 pre-RA MIR，但每个 allocator 可以拥有 splitting/spilling/rematerialization/coalescing 策略；post-RA pipeline 相同。
回答“完整 RA 系统最终生成的代码谁更快”。

比赛最终目标优先 RA-System；算法研究和归因使用 RA-Core。

## 3. 自研 allocator 组

至少保留：

- `linear`：简单、快速 baseline；
- `greedy`：静态/简单图着色 baseline；
- `ssa-greedy`：interference + affinity + dynamic saturation/priority；
- 后续实验 allocator 通过同一接口注册，不改变 pipeline ownership。

```cpp
AllocationResult run(
    MachineFunction&,
    const TargetRegisterInfo&,
    const AllocationOptions&);
```

`AllocationResult` 至少记录 assigned/spilled/rematerialized、spill slots、callee-saved activation、compile time 和 metrics。

## 4. LLVM RA Lab

用同一 `.ll`、同 target、同 LLVM 18 版本，在 LLVM 自己的 machine pipeline 内冻结其 pre-RA MIR，再比较 LLVM 可选择的 allocator。
目标对照包括 Basic、Greedy、PBQP；Fast 可作为快速 baseline，但若它使用不同 surrounding pipeline，则不能冒充严格 core-controlled 对比。

理想的后续实验是写一个薄 LLVM adapter，把“我们的 allocation engine”接到 LLVM MachineFunction/LiveIntervals/VirtRegMap 环境中。
这只用于离线研究：

```text
             Our Allocation Engine
             /                   \
Own MIR adapter                   LLVM adapter
```

不要尝试把 LLVM Greedy 强行抽象成我们的 `AllocationProblem`；它的 splitting/eviction/live-range machinery 属于 LLVM RA system 本身。

## 5. 双实验室，不混淆 MIR

```text
same SysY corpus
   |
   +-- Own Backend Lab
   |      Frozen Own MIR
   |      linear / greedy / ssa-greedy / ...
   |
   `-- LLVM RA Lab
          Frozen LLVM MIR
          Basic / Greedy / PBQP / OurRA adapter

                 -> shared metrics DB/dashboard
```

Own MIR 与 LLVM MIR 不同，所以跨两个实验室的结果不能解释成“纯 RA 算法差异”。
另保留 End-to-End reference：Own backend + OwnRA vs LLVM backend + Greedy；它回答完整 backend gap，而不是 RA-isolated gap。

## 6. RA 质量指标

不要只看“spilled vreg 数”。至少记录：

- logical spill value count；
- static spill store / reload count；
- weighted spill memory traffic（使用 BFI，未就绪时 loop-depth proxy）；
- rematerialization count；
- spill frame bytes / peak frame；
- copy/MOV before and after RA；
- satisfied affinity weight / weighted copies eliminated；
- caller-saved / callee-saved activation；
- live-across-call value handling；
- cross-bank move（若启用相关实验）；
- machine instructions/code bytes after real assembly；
- RA compile time / peak memory。

最终仍以 runtime/cycles 为裁判。

## 7. Synthetic 与 exact corpus

三层 corpus：

1. synthetic MIR：压力围绕 K-1/K/K+1/K+2/2K、跨 call live、PHI join/cycle、bank imbalance、rematerializable constants、长短 live range 混合；
2. competition workloads：真实热函数和整程序；
3. small exact solver：对小 interference problem 用离线 brute force/ILP/SAT 求最优或下界。

exact solver 的目标应为 lexicographic：legality -> weighted spill cost -> affinity/copy cost -> callee-save activation 等。
它是 allocator heuristic 的算法质量参照，不是假装预测目标机 runtime。

## 8. Runtime measurement

每个 benchmark 保留 raw samples，warmup 后多次运行；至少报告：

- median 与 spread/置信区间；
- target-specific geometric mean；
- wins/ties/losses；
- worst regression；
- p50/p90 regression；
- per-case delta。

运行顺序交错/随机化以减小温度/频率漂移。可用时记录 cycles、instructions、loads/stores、branches、branch misses、cache events。
A53 与 BOOM 分开汇总；不要把两台机器压成一个单一总分。

## 9. Machine-readable schema

最小记录示例：

```json
{
  "experiment": "ra-lab-v1",
  "target": "aarch64-cortex-a53",
  "benchmark": "case-id",
  "host": "own-mir",
  "mode": "ra-system",
  "allocator": "ssa-greedy",
  "source_hash": "...",
  "input_hash": "...",
  "pre_ra_hash": "...",
  "target_config_hash": "...",
  "post_pipeline_hash": "...",
  "ra_time_us": 0,
  "vregs": 0,
  "spilled": 0,
  "spill_loads_static": 0,
  "spill_stores_static": 0,
  "weighted_spill_traffic": 0,
  "rematerialized": 0,
  "copies_before": 0,
  "copies_after": 0,
  "spill_frame_bytes": 0,
  "machine_insts": 0,
  "code_bytes": 0,
  "runtime_ns_samples": []
}
```

还要记录 compiler commit、LLVM version（LLVM lab）、assembler/linker version、target triple/cpu/features/ABI、machine identity、run/warmup index。

## 10. Dashboard

静态本地报告即可，不要求服务器：

```text
tools/ra_lab/report/
  index.html
  data.json
  summary.json
```

页面至少包括：

- Overview：每个 target 的 normalized runtime/geomean/worst regression；
- Heatmap：allocator × benchmark；
- Spill：weighted load/store、remat、frame、copies；
- Pareto：allocator compile time vs generated-code runtime；
- Per-function explorer：函数 pressure/spill/copy/size 与 pre/post RA MIR、asm diff；
- Experiment Purity：source/input/target/pre-RA hash/post-pipeline/linker/runtime machine 全部打勾才显示“RA-isolated”；
- Statistics：raw samples、median/CI、win/tie/loss；
- Prediction：若接入 cost model，显示 predicted pressure/spill 与 actual error。

## 11. Ablation

必须把 allocator 与 spill policy 分开实验，例如：

```text
allocator: linear / simple-greedy / ssa-greedy
x
spill: stack-only / +remat / +slot-color / optional target cross-bank
```

否则“SSA greedy 赢了”可能实际只是 rematerialization 赢了。

Scheduler/ISel/BlockPlacement 共享同一实验 runner，但使用各自 freeze boundary 和 purity rule：

```text
--scheduler=identity|latency|pressure|hybrid
--isel=direct-combine|pattern
--block-placement=original|loop|frequency
```

不要为每个 pass 重造数据格式和可视化框架。

## 12. 建议目录

```text
tools/perf_lab/
  runner/
    experiment.py
    manifest.py
  adapters/
    own_backend/
    llvm18/
  metrics/
    mir_metrics.py
    asm_metrics.py
    runtime_metrics.py
  dashboard/
    generate_report.py
    template/
  schemas/
    experiment.schema.json
  corpus/
    synthetic/
    exact_solver/
  results/          # 不提交大结果
```

原则：**统一实验协议 != 统一内部 IR**。统一 identity、freeze、metrics、runtime 方法和 purity；不要为了实验方便把 LLVM MIR 与 Own MIR 强行同构。
