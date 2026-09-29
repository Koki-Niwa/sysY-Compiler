# SysY Compiler — 前端与中端

自研 C++17 SysY2022 编译器前中端。产品目标是在正确性与资源约束内，使最终生成汇编的运行性能尽可能高；工程原则是 **Codex 可读性第一、功能模块化设计**。

多层 IR 保留循环、访问和控制流信息，为后续优化 pass、多面体优化、e-graph 与全程序优化提供边界。计划交付目标无关的 LLVM IR 子集文本，指令选择、寄存器分配与汇编发射由后端负责。

## 当前能力

已实现前端、结构化 IR、循环规范化、FlattenCFG、平面 IR 及其读回/验证，并能将经过验证的平面 IR 发射为目标无关的 LLVM IR 子集文本。`--from-flat` 可把平面 IR 文本作为发射输入；发射失败时报告诊断且保留既有输出文件。`-O1` 当前只记录选项，没有启用优化。

完整事实与阅读路径：

- [当前状态](docs/CURRENT-STATE.md)
- [目标](docs/GOALS.md)与[架构及模块契约](docs/ARCHITECTURE.md)
- [性能扩展与测量契约](docs/PERFORMANCE-EXTENSIONS.md)
- [开发约定](AGENTS.md)

## 构建与使用

从本仓库目录执行；要求 C++17、CMake ≥ 3.20、Ninja，无第三方产品依赖。既定验收工具链为 `clang++`。

```bash
cmake -S . -B build -G Ninja
cmake --build build
build/compiler input.sy --emit=structured-ir -o output.sir
build/compiler input.sy --emit=structured-ir --normalize -o normalized.sir
build/compiler input.sy --emit=flat-ir -o output.fir
build/compiler --help
```

当前可用产物：默认 `llvm-ir`、`tokens`、`ast`、`sema`、`initplan`、`structured-ir`、`flat-ir`、`nothing`。
`--from-ast`、`--from-structured`、`--from-flat` 用于读回对应产物；完整选项以 `--help` 为准。

退出码：`0` 成功，`1` 编译或发射错误，`2` 命令行用法错误，`3` 文件读写错误。

## 模块

| 目录 | 职责 |
|---|---|
| `src/frontend/` | 词法、AST、语义、常量与初始化计划 |
| `src/structured/` | AST-to-IR 桥、结构化 IR、循环规范化、展平 |
| `src/ir/` | 平面 IR 数据模型、读回、验证与 LLVM 文本发射 |
| `src/support/` | 源码位置、诊断、中立运行库签名 |
| `src/main/` | CLI 与管线组合 |
| `tools/` | 开发期验证；`verify.sh` 协调 `gate/` 各检查组 |

`analysis/` 与 `opt/` 是未来扩展方向，当前没有实现通用分析/优化框架。

## 关键语义

- 整数为 32 位回绕语义，不发射 `nsw`/`nuw`；浮点不使用 fast-math。
- 前端变量读保留显式 load，由后续 mem2reg 负责提升。
- 结构化到平面 IR 按管线单向降级，每个边界验证不变量。
- 计划交接的 `.ll` 不带 triple/layout 或主机 CPU 属性，由后端提供目标配置。
- `tensor` 语法暂不支持；当前官方清单中 490 个用例在范围内，50 个明确排除。

## 验证

```bash
python3 tools/selftest/check_architecture.py --root .
python3 tools/selftest/check_line_budget.py --root .
python3 tools/selftest/check_cli_capabilities.py
bash tools/verify.sh --stage S07
```

完整关卡需要工作区的同级 `tests/`、`runtime/` 和开发工具链；本仓库可独立构建并运行上述三个局部检查。`--quick` 仅用于快速反馈，不是完整阶段验收。`tools/gate/*.sh` 由协调器加载，不独立运行。

## 实现边界

产品不链接 libLLVM、不调用 `clang`/`llc`/`opt` 等外部程序，不使用现有编译器框架源码。开发期测试可调用独立工具验证正确性；研究资料仅用于理解设计。
