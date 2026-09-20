# SysY Compiler — 前端与中端

一个 **SysY → LLVM IR 子集** 编译器的**前端与中端**实现。

- **输入**：SysY 源文件（`.sy` / `.sysy`）
- **输出**：目标无关的 LLVM IR 子集文本（`.ll`）
- **边界**：本项目**到 `.ll` 为止**。指令选择、寄存器分配、汇编发射由后端负责。

---

## 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
# 产物：build/compiler
```

**要求**：C++17、CMake ≥ 3.20、任意标准库实现（`clang++` / `g++`）。
**无第三方依赖**。

---

## 用法

```bash
build/compiler <input.sy> -o <output.ll> [选项]

选项：
  -o <file>          输出文件（必填）
  --emit=<kind>      产物类型: llvm-ir（默认）| tokens | ast | structured-ir | nothing
  -O0 / -O1          优化级别（-O1 目前只记录，后续生效）
  --optimize         -O1 的别名
  -S                 接受但不解释（比赛调用形式: compiler a.sy -S -o a.s）
  --structured       使用结构化 IR 层（默认）
  --no-structured    不走结构化 IR 层（供回归对照用）
  --toy-backend      接受但不解释（测试链路选择：走自研降级器而非 clang）
  --verbose          打印处理过程
  -h, --help
  -v, --version
```

**退出码**：`0` 成功 · `1` 编译错误 · `2` 命令行用法错误 · `3` 文件读写错误

---

## 设计要点

| 项 | 说明 |
|---|---|
| **两层 IR** | ① 结构化 IR（循环是 Op、带 Region、无 SSA、变量在内存）→ `FlattenCFG` → ② 平面 CFG IR（SSA + φ） |
| **目标无关的 `.ll`** | 输出**不带** `target triple` / `datalayout` / `target-cpu` / `target-features`；目标由降级阶段决定 |
| **整数语义** | `int` 是 32 位、溢出按 2³² 回绕；**不打** `nsw`/`nuw` |
| **浮点语义** | **不加** `fast`/`reassoc`/`nsz`/`contract`（评测精确比对输出） |
| **地址计算** | 下标算术在 `i32` 里做（允许回绕），算完 `sext` 到 `i64` 再进 `getelementptr` |
| **未定义行为** | 跨目标规范化（如 `fptosi(NaN)` → 0、`sdiv` 除零 → 0） |

---

## 目录

```
src/
├── main/       命令行入口
├── support/    诊断引擎、源文件读取、SourceLoc
└── ...         前端 / IR / 分析 / 优化（随开发推进填充）
tools/          开发用脚本（测试运行器、自检链路）
```

---

## 状态

| 阶段 | 状态 |
|---|---|
| 骨架与验证基础设施 | ✅ 完成 |
| 词法分析 | 进行中 |
| 语法 / 语义 / 两层 IR / 优化 | 计划中 |

---

## 开发工具

`tools/` 下的脚本用于本项目的回归测试与链路自检。
它们**不是编译器的一部分**，只在开发环境使用（需要 `clang` / `llvm-as` / `llc`，
以及 `runtime/sylib.c`——那是**比赛官方提供的运行时库**，不随本仓库分发）。

---

## 合规声明

本项目为**从零实现**，**未使用** GCC / LLVM 等现有编译器及其框架的源代码。
编译器的运行**不依赖任何外部程序**。

`clang` / `llvm-as` / `llc` 等工具**仅出现在开发期的测试脚本中**，
用于独立校验产出的 IR 是否合法、以及交叉验证目标平台行为——
**不参与编译流程，也不随交付物分发**。
