# `compiler/tests/structured/` —— S05 的用例集

`run_structured_cases.py --compiler <路径>` 跑全集，退出码 0/1。

## `example/` —— **格式契约**（关卡 `cmp -s` 冻结）

| 文件 | 说明 |
|---|---|
| `example.sy` | 源码（覆盖 §六 要求的形态） |
| `example.emit-structured.txt` | **真实运行产出**的 `--emit=structured-ir` 转储 |

⚠️ **不要手写期望输出**。S03/S04 的转储格式都因为"手写示例与实际输出不一致"
被关卡抓过。这里的转储是真实运行产出的；改格式必须**重新生成**它。

## `golden/<编号>-<短名>/` —— 金样例

沿用 S03/S04 的结构：`good.sy`（源码）+ `expect`（**编译器的真实输出**）。
每个用例覆盖一类降级形态，名字说明它测什么：

| 编号 | 覆盖 |
|---|---|
| 01 | 标量算术 + 铁律 1（`return` 的操作数是 `load`，不是常量） |
| 02 | `int` / `float` 混合与 `Cast`（`I2F`/`F2I` 饱和/`ToBool`） |
| 03 | 数组与多维下标（i32 下标运算 → `sext` → `GEP`） |
| 04 | 全局（`:zero` 与 `:data`）+ `GetGlobal` |
| 05 | 局部初始化（`Zero`/`StoreConst`/`StoreExpr`/`MemcpyConst`） |
| 06 | `if` / `else` |
| 07 | `while` |
| 08 | 函数调用（含数组实参） |
| 09 | `starttime` / `stoptime`（行号作为实参） |
| 10 | `/` 与 `%` 的**归一化守卫** |
| 11 | `fptosi` 的**饱和** |
| 12 | 短路 `&&` / `||` |

## `min/<编号>-<短名>/` —— 最小对照

沿用 S03/S04 的 `bad.sy` / `good.sy` / `expect`（**只差一处**）。
S05 **不产生新诊断**（语义检查在 S03 做完了），所以这里的判据是
**"不许崩、不许静默产出空 IR"**：`bad.sy` 允许带 S03 已有的诊断（退出码 1），
但**必须仍然产出合法且非空的结构化 IR**（轨 B 会验），而 `expect` 写的是
现有诊断编号（如 `E-UNDEF 3`），空文件表示"零诊断"。
