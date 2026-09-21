# `loopnorm/example/` —— `--normalize --emit=structured-ir` 的**格式契约**

`example.sy` 与它**真实运行产出**的 `example.emit-structured.txt`，
由**关卡**用 `cmp -s` 逐字节比对（与 `structured/example/`、
`sema_dump_example/`、`initplan_example/` 同一做法）。

## 为什么必须有它

`--normalize` 会改写 IR（`while` → `for`、`continue` 消解、`alloca` 位置），
改写后的**文本形态就是对外契约**。prompt §四：*"不要手写例子当契约 ——
S03/S04 各栽过一次，两次都是关卡里的样例对比抓出来的。"*
⇒ 这里的 `.txt` 是**真实运行产出**，不是手写的。

## 这个例子覆盖了什么

* `sum_upto`：**变体 1** 的形状（`if (C) { i=i+1; continue; }` 在体内，
  递增路径两条）⇒ 产出 `For` + 一个 `if (!C) { 剩余体 }` 包装；
* `main` 的第一个 `while`：**变体 3** 的形状（初值 `i = 1`、边界 `N - 1`）
  ⇒ `For` 的 `lower` 是"进入循环时的 IV 值"、`upper` 是 `N - 1` 的结果；
* `main` 的第二个 `while`：平凡循环（只有自增、无其他语句）；
* 三个 `while` 全部升为 `For`，所以这份样例**同时**是"规范化生效"的证据。

## 格式（与 S05 的 `structured-ir` 同一套规则，只有 `For` 是 S05b 定稿的）

```
(For "<IV 的结果名>" <IV 槽> <lower> <upper> <step> @line <n>) {
  ... 体 Region（以 (Yield) 收尾；体内**没有** Break、**没有** IV 的自增）...
}
```

* `attrs[0]` = IV 的名字（**字符串**，给人读的；以 pass 运行时的名字为准，
  不随 dump 的结果重编号而变）；
* 操作数 0 = IV 的槽（`AllocaOp` 的结果，**身份**；I1 的判据）、
  1 = `lower`（进入循环时 IV 的值）、2 = `upper`（边界表达式的结果）、
  3 = `step`（本关恒为 `1`）；
* 语义：`iv = lower; while (iv < upper) { body; iv += step; }`
  （**先判后执行**，`lower >= upper` 时体一次都不执行）。
