# 验证关卡的模块边界

入口仍为 `bash compiler/tools/verify.sh --stage S06`。脚本从自身位置寻找工作区，官方 `tests/` 与 `runtime/` 仍须位于源码仓库旁。

协调器持有 `ROOT/TOOLS/TESTS/RUNTIME/COMPILER/LLVM_BIN/WORK/STAGE/QUICK`、结果计数器及能力/执行证据位。按显式顺序 source 下表模块；模块使用 `c_ok/c_bad/c_skip/hdr` 报告结果，不能调用下一模块。加载失败直接失败。数据是本次进程内状态，不能用旧日志填充。

## 从旧单文件到模块的逐组映射

基线为源码仓库 `7a88fd7:tools/verify.sh`（842 行）。旧 A–C8 的检查按连续区间移动，保留原条件、冻结样例、坏输入反证和语料门槛。

| 原组 | 新文件 | 保留的检查 |
|---|---|---|
| A | `build.sh` | CMake/零警告、全部单元测试、RSS 预算、行数预算 |
| B | `compliance.sh` | 外部进程、函数名特判、测试路径三项检查 |
| C/能力探测 | `llvm.sh` | 真实定义、llvm-as、opcode、alloca、目标属性、inbounds、nsw/nuw、浮点比较 |
| C2–C4 | `frontend.sh` | 词法拼接/无崩溃；AST 往返、独立 parser、冻结指纹；sema 样例、不变式/反证/用例 |
| C5 | `initializers.sh` | 初始化能力/格式、冻结样例、全量计划检查、用例；验证工具不得依赖未提供源码的字节码 |
| C6 | `structured.sh` | 冻结样例、结构化不变式/反证、独立 IRGen、用例 |
| C7 | `loops.sh` | 规范化不变式、独立规范化、行为对照、最小用例 |
| C8 | `flat.sh` | 冻结平面样例、清单身份/数量、坏 IR 反证、独立展平、行为对照、回边普查、平面用例 |
| C8a | `flatverifier.sh` | S08 起通过公开 `verifyFlatModule` API 运行坏/合法 IR 矩阵，并核对旧 8 个坏例映射 |
| C8b | `mem2reg.sh` | S09 起对提升后的 flat IR 重跑 structured/flat 可观察行为对照，锁定与 fresh S08 基线相同的覆盖和差异数 |
| D/E | `execution.sh` | 三个 target、no-structured、自研降级器执行；新增逐文件 JSON 证据校验 |
| E2 | `interpreter.sh` | S10a AST 解释器固定/生成用例、资源限制、重放和负控 |
| F | `stages.sh` | 显式阶段顺序及累积能力要求；S07/S10a/S11/S10b/S11b 均有必需证据位 |
| 新 A2 | `architecture.sh` | 依赖方向/环、CLI 能力契约、三个判据反证套件 |

原来由 `--quick` 跳过的独立 IRGen、独立规范化、规范化行为、独立展平和平面行为仍按原条件跳过。快速检查成功只输出 `QUICK/SMOKE PASS — NOT FULL ACCEPTANCE`。

## 有意加强的标准

- LLVM 输出仅接受真实定义，或者退出码 2 加 `error[E-UNSUPPORTED-LLVM]` 的明确未实现诊断；其他失败不是缺能力。S07 起还必须有 llvm-as 成功证据。
- S11 起三个 target 与 no-structured 的执行证据全部必需；缺 qemu 等会留下未完成证据位，阶段验收失败。
- S10a 要求 AST 解释器自身的固定用例、生成器和负控证据；旧 `--stage S10` 名称仍作为 S10a 别名接受。
- S08 要求公共 FlatVerifier 边界矩阵：至少 12 个不同无效 IR、6 个合法形状和原 8 个坏例映射。旧文本检查器报红不能替代 API 级拒绝证据。
- S09 要求 fresh promoted-flat/structured behavior evidence；`--quick` 跳过该对照时 `MEM2REG_OK` remains unset and the requested S09 gate fails.
- S10b 排在 S11 之后，要求 S11 的完整目标执行证据与独立 AST/目标差分证据；解释器本身不依赖 S11。
- `check_execution.py` 校验运行器 `--json` 对应的 target、540 个清单身份、490 个必需通过、50 个指定 tensor 排除、逐条状态与汇总一致。运行器退出 0 不足以验收。
- S11b 及以后额外要求自研降级器的同范围执行证据。
- 产品 `.inc` 纳入 600 行上限；协调器与 `gate/*.sh` 的 600 行上限不能被棘轮覆盖。其他旧工具的棘轮不因此整体刷新。

局部反证：`python3 compiler/tools/selftest/test_architecture_gate.py`、`test_execution_gate.py`、`test_stage_gate.py`（后两项使用相同目录前缀）。测试包含实际 shell 执行接线，全跳过/退出 0 在普通及 toy 路径均必须失败。

全部脚本目前可读。判据独立性依靠反例、保留基线和独立复核，不依靠声称隐藏脚本。新增门禁应先有会失败的反例；变更阶段范围必须同步当前状态与验收文档。

架构检查覆盖引号与角括号形式的本地字面量 include；系统头不进入项目图，不声称解析宏展开等动态依赖。阶段门禁检查上表列出的能力证据，设计审计报告、性能收益等仍须按阶段 prompt 验收。S05a 是 S05 实现前的表达能力审计，因此排在 S05 之前。
