# `compiler/tests/flat/` —— flat（平面层）用例集

给 **S06「平面 IR + FlattenCFG」** 准备的语料：把"扁平层要吃的那份输入形状"
逐字节钉死。跑法：

```bash
python3 compiler/tools/selftest/run_flat_cases.py --compiler compiler/build/compiler
```

> ⚠️ **当前档位是 `--emit=structured-ir --normalize`，不是 `--emit=flat-ir`。**
> 建这份语料时 `--emit=flat-ir` 还没实现（编译器回
> `error: unknown --emit kind 'flat-ir'`，退出码 2），所以先按**结构化层**判，
> 覆盖的正是扁平层必须正确处理的那些形态：φ 位点、嵌套合流、循环规范化结果、
> 临界边、`alloca` 位置、不可达代码、以及"还没有 mem2reg"的证据。
> S06 的 `--emit=flat-ir` 落地后，把
> `run_flat_cases.py` 顶部的 `EMIT_ARGS` / `ROUNDTRIP_ARGS` 换成 flat 档，
> 再跑一次 `--update` 重刷全部 `expect` 即可 —— 判据与目录结构都不用改。

## 目录布局

```
tests/flat/
  README.md                     ← 本文件
  golden/<NN>-<短名>/
      case.sy                   ← 源码（合法 SysY，零诊断、退出码 0）
      expect                    ← **真实运行产出**的转储，不许手写
      README                    ← 一行：这个用例钉住什么（dump 里必须成立什么）
  min/<NN>-<短名>/
      good.sy / bad.sy          ← **只差一处**（统一 diff 恰好 1 个 hunk）
      good.expect / bad.expect  ← 两份源码各自的真实产出
      README                    ← 一行：差在哪一处 + dump 里可观察的差别
```

### `golden/` 与 `min/` 的分工

| | `golden/` | `min/` |
|---|---|---|
| 形态 | `case.sy` + `expect` | `good.sy` / `bad.sy` + 两份 `expect` |
| 回答的问题 | "这个形态**长什么样**"（形状契约） | "**这一处**改动让 dump 变了什么"（因果最小对照） |
| 判据 | 与 `expect` 逐字节相同 + 往返逐字节相同 + stderr 零诊断 | 同左（两份各判一次）+ 两个源文件**恰好 1 个 hunk** |
| 用途 | 冻结形状，改格式必须重新生成 | 证明某个形态判据**真的可观察**（不是"看起来对"） |

`golden/` 的 14 个用例（编号 = 目录名前缀）：

| 编号 | 钉住 |
|---|---|
| 01 | 直线代码 + `int`/`float` 互转（`I2F`、`F2I` 饱和守卫） |
| 02 | `if`/`else` 合流 —— **φ 位点**（合流后读同一个槽） |
| 03 | 嵌套 `if`（φ 的入值本身是另一个 φ 的结果） |
| 04 | `while`（`--normalize` 后成 `For`） |
| 05 | `break`（真 break ⇒ 保留 `While` + `BreakOp`） |
| 06 | `continue`（消解成 `if (!cond) { 剩余体 }`） |
| 07 | 多级嵌套循环 |
| 08 | 函数调用（非 void 被调 / void 被调 / void 运行时调用） |
| 09 | 数组（全局、局部、`a[i][j]`、数组实参） |
| 10 | `alloca` 位置（循环体内声明 ⇒ Alloca 仍在入口 Region） |
| 11 | 临界边（2 后继 × 2 前驱） |
| 12 | `return`/`break` 之后的不可达代码（不产生 Op） |
| 13 | **不做 mem2reg 的证据**（store 之后紧跟 load） |
| 14 | `starttime`/`stoptime`（行号作为实参） |

`min/` 的 10 组：φ 读/不读、循环边界可提升/不可提升、声明在循环内/外、
`continue`/`break`、嵌套/单层 `if`、IV 循环后读/不读、变量下标/常量下标、
全局零初始化/数据初始化、浮点乘/整数乘、`while`/`if`。
每组差在哪一处、dump 里能看出什么，见该目录的 `README`。

## 生成 / 刷新 `expect`（**唯一合法来源**）

单个用例（`golden`）：

```bash
compiler/build/compiler compiler/tests/flat/golden/13-no-mem2reg/case.sy \
    --emit=structured-ir --normalize -o compiler/tests/flat/golden/13-no-mem2reg/expect
```

单个用例（`min`，两份各一条）：

```bash
compiler/build/compiler compiler/tests/flat/min/04-continue-vs-break/good.sy \
    --emit=structured-ir --normalize -o compiler/tests/flat/min/04-continue-vs-break/good.expect
compiler/build/compiler compiler/tests/flat/min/04-continue-vs-break/bad.sy \
    --emit=structured-ir --normalize -o compiler/tests/flat/min/04-continue-vs-break/bad.expect
```

全部（**推荐**，`--update` 只给维护者用）：

```bash
python3 compiler/tools/selftest/run_flat_cases.py --compiler compiler/build/compiler --update
```

⚠️ **绝对不要手写 `expect`。** 转储的排版（列顺序、属性顺序、缩进）是**真实输出
定义的契约** —— S03/S04/S05 都栽在"手写示例与实际输出不一致"上。改了 dump 格式，
就重新生成 `expect`，不要改文字。

## 判据（`run_flat_cases.py`）

`golden/*/case.sy` 与 `min/*/good.sy|bad.sy` 每一个都要过三条：

1. **编译**：退出码 0，且 **stderr 零诊断**（有诊断即失败）；
2. **对照**：产物与对应 `expect` **逐字节相同**；
3. **往返**：产物再走 `--from-structured --normalize --emit=structured-ir`
   读回 dump，必须与产物**逐字节相同**。

`min/` 另加：`good.sy` 与 `bad.sy` 的统一 diff **恰好 1 个 hunk**（`difflib` 口径，
与 `diff -u` 相同）。

退出码：`0` 全过 · `1` 有用例失败 · `2` 工具自身错误（找不到编译器/用例树）。

## 已知边界（写下来，免得后来人踩）

* `--emit=flat-ir` 尚不存在，见文首的说明。
* 结构化层**还没有 mem2reg**（`golden/13` 就是这件事的证据）：变量一律
  `alloca`/`load`/`store`，合流处只有"槽的再读"，没有 φ。
* `IRGen` 把 `break` 与 `continue` **都**降级成 `BreakOp`，靠形状推断区分。
  于是
  `while (c) { if (x) { <含自增的语句>; break; } ... }`
  这种"`break` 在 `if` 分支末尾、分支里恰好有一次自增"的写法，会被
  `LoopNormalize` 当成 `continue` 升成 `For`（**语义变了**）。本语料
  **不依赖**这个行为：`golden/05`、`golden/11`、`golden/12` 的 `break` 分支里
  没有自增（保留 `While + Break`，语义正确），`min/04` 用的是能区分的形状。
