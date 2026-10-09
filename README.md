# SysY Compiler

自研 C++17 SysY2022 编译器。目标是在正确性、ABI 与资源约束内，逐步建立可独立优化、最终可独立生成 AArch64 / RV64GC 汇编的完整编译链。

当前产品已经能够把支持范围内的 SysY 程序编译为经过验证的目标无关 LLVM IR 文本；默认路径为 O0，`-O1` / `--optimize` 启用当前优化管线（目前为 Mem2Reg）。正式自研机器后端仍在后续路线中，现阶段 LLVM 工具只用于独立验证和执行测试。

## 当前状态

当前支持的官方测试清单为 540 项，其中 490 项属于已支持语言范围，50 项 `tensor` 用例按设计拒绝。

已经完成并正式验收：

- 前端：lexer、parser、AST、语义检查、常量求值与初始化计划。
- Structured IR、循环规范化、FlattenCFG。
- Flat IR、序列化/读回、FlatVerifier、稳定 dump。
- 目标无关 LLVM IR 文本发射；490 个 supported case 已有完整 LLVM assembly / execution 验证历史。
- S11：x86、AArch64、RV64 与 serialized-Flat boundary 的 revision-bound correctness evidence。
- S11b：独立 self-built x86 toy lowering，可直接消费冻结的 LLVM IR 子集；它是 correctness/protocol consumer，不是正式性能后端。
- F00：一等 `Value/Use{user, operandIndex}`、独立 `Argument`、canonical `Constant`、精确 RAUW，且 490/490 Flat 文本与旧协议逐字节一致。

当前正在收尾 F01：

- `IRMutator`：正常优化器的 tracked data-flow mutation authority。
- `CFGEditor`：Phi / CFG edge / predecessor cache 的原子修改入口。
- Instruction 生命周期：`Detached / Live / Tombstone`。
- Module / Function revision 与 `MutationSummary`。
- Mem2Reg 已迁移为上述 mutation layer 的真实 consumer。
- optimizer raw-mutation architecture gate 已收紧；F01 实现与短验证完成，尚待最终独立长验证后才能标记 CLOSED。

完整、最新的冻结事实以 [CURRENT-STATE](docs/CURRENT-STATE.md) 为准。路线和边界见 [ARCHITECTURE](docs/ARCHITECTURE.md)、[GOALS](docs/GOALS.md) 和 [BACKEND-PERFORMANCE-PLAN](docs/BACKEND-PERFORMANCE-PLAN.md)。

## 构建

要求 C++17、CMake ≥ 3.20、Ninja；产品本身不依赖 LLVM 库。

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

主要可执行文件：

```text
build/compiler
build/toy_backend
```

`toy_backend` 只用于 S11b 的 x86 correctness/protocol 验证，不代表正式后端架构。

## 基本使用

直接生成 LLVM IR：

```bash
build/compiler input.sy --emit=llvm-ir -o output.ll
```

启用当前 O1（Mem2Reg）：

```bash
build/compiler input.sy --emit=llvm-ir -O1 -o output.opt.ll
```

查看或保存中间 IR：

```bash
build/compiler input.sy --emit=structured-ir -o output.sir
build/compiler input.sy --emit=structured-ir --normalize -o normalized.sir
build/compiler input.sy --emit=flat-ir -o output.fir
```

从冻结的 Flat 文本重新进入产品管线：

```bash
build/compiler output.fir --from-flat --emit=llvm-ir -o output.ll
build/compiler output.fir --from-flat --emit=llvm-ir -O1 -o output.opt.ll
```

完整选项：

```bash
build/compiler --help
```

当前可用产物包括默认 `llvm-ir`、`tokens`、`ast`、`sema`、`initplan`、`structured-ir`、`flat-ir` 和 `nothing`。`--from-ast`、`--from-structured`、`--from-flat` 用于从对应序列化边界重新进入管线。

退出码：`0` 成功，`1` 编译/验证/发射错误，`2` 命令行用法错误，`3` 文件读写错误。

## 代码结构

| 目录 | 职责 |
|---|---|
| `src/frontend/` | 词法、AST、语义、常量与初始化计划 |
| `src/structured/` | AST-to-IR 桥、Structured IR、循环规范化、展平 |
| `src/ir/` | Flat IR、Value/Use、mutation/revision、读回、验证、LLVM 文本发射 |
| `src/opt/` | 当前优化实现；目前包含 Mem2Reg |
| `src/support/` | 源码位置、诊断、中立运行库签名 |
| `src/main/` | CLI 与当前管线组合 |
| `tools/toy_backend/` | 独立 C++17 x86 toy lowering，仅用于 correctness/protocol 验证 |
| `tools/selftest/` | 单元、架构、mutation boundary 与 focused selftests |
| `tools/gate/` | 阶段 gate；由 `tools/verify.sh` 协调 |

下一阶段 F02 才会加入 revision-aware / dependency-aware `AnalysisManager`；F03 才会建立正式 `FlatPassManager`。不要把当前 `src/opt/` 理解为已经具备完整通用 pass infrastructure。

## 关键语义与边界

- 普通 SysY 整数按 32 位回绕语义处理，不为普通整数算术发射 `nsw` / `nuw`。
- 浮点不启用 fast-math。
- SysY 普通二元操作数的求值顺序在语言层面未指定；本项目内部采用确定性的 lhs → rhs。
- `&&` / `||` 保持短路语义。
- 默认 O0 保留显式内存操作；O1 当前由 Mem2Reg 提升满足条件的局部标量。
- 发射的 `.ll` 不携带目标 triple、data layout 或主机 CPU 属性；目标配置由后续 lowering/execution 环节提供。
- `tensor` 当前不属于支持范围。
- 正式后端路线最终是 in-memory Flat → BackendLowering → MIR → legalization / ISel / RA / scheduling / frame / layout → GAS；toy backend 不会演化成这条正式产品路径。

## 验证

常用短门禁：

```bash
bash tools/selftest/unit/run.sh
python3 tools/selftest/check_architecture.py --root .
python3 tools/selftest/check_ir_mutation_boundary.py --root .
python3 tools/selftest/check_line_budget.py --root .
python3 tools/selftest/check_cli_capabilities.py --compiler build/compiler
```

完整阶段验证由：

```bash
bash tools/verify.sh --stage <stage>
```

协调执行。完整关卡需要工作区同级的 `tests/`、`runtime/` 与对应 LLVM / cross / QEMU 开发工具链。`--quick` 只用于快速反馈，不代表阶段正式验收。

## 实现边界

产品编译器不链接 libLLVM，也不通过调用 `clang`、`llc`、`opt` 来完成自身的前端/中端工作。开发期 gate 可以使用 LLVM、cross compiler、QEMU 等外部工具作为独立 oracle、目标 lowering 或执行环境。

当前正式产品尚未实现最终 AArch64 / RV64 机器后端；其实现将按既定 B00–B20 路线推进。
