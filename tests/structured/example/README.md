# `structured_example/` —— `--emit=structured-ir` 的**格式契约**

`example.sy` 与它**真实运行产出**的 `example.emit-structured.txt`，
由**关卡**用 `cmp -s` 逐字节比对（与 S03 的 `sema_dump_example/`、
S04 的 `initplan_example/` 同一做法）。

## 为什么必须有它

提示词 §六 的格式规则是**文字**，而文字会漂。S03 的转储格式曾经
"例子与规则文字自相矛盾"、S04 的示例与实际输出有三处排版差异 ——
两次都是**关卡里的样例对比**抓出来的。⇒ **先写规则，再让真实输出定义排版**，
两者不一致时**以样例对为准**。

## 格式规则（真实输出的摘要；细节看那个 .txt）

```
(Module "<源文件基名>") {
  (GlobalVar "<名字>" :type <类型> :init "<zero|data>" [{偏移=值, ...}] @line <n>)
  (GetGlobal "<名字>" %<模块级序号> :type <类型> @line <n>)
  (Func "<名字>" :ret <类型> :param [<类型>, ...] @line <n>) {
    (OpKind %<函数名>.<序号> ...操作数... ...属性... @line <n>)
    (While @line <n>) {
      ... 条件 Region（以 `(Yield %cond)` 收尾）...
    }
    {
      ... 体 Region（以 `(Yield)` 或 `(Break)` 收尾）...
    }
  }
}
```

**一行的顺序**（读回器按同一个顺序吃，**这是契约**）：

1. `(` OpKind
2. `Call` / `GetGlobal` 的**名字字符串**（它们要在结果之前：`Call` 的被调函数
   决定"有没有结果"）
3. **结果列表**（0..N 个 `%<函数名>.<序号>`；多结果就打印多个）
4. 其余头部属性（`Module`/`Func`/`GlobalVar` 的名字、`:ret`/`:param`/`:type`/`:init`）
5. 其余属性（`Int`/`Float` 的值、`GEP` 的元素类型/下标类型/亲和性标记、
   `GlobalVar` 的 `{偏移=值}` 表）
6. **操作数**（一律引用结果名 `%f.N`；**常量也是 Op、也有名字**，不内联）
7. `@line <行号>`（D11：SourceLocation 一路带到 IR；`starttime` 依赖它）

**结果的命名**：`%<函数名>.<序号>`，序号在**函数内**从 0 递增；
模块级 Op（`GetGlobal`）的结果用 `%.<序号>`。

**类型拼写**：`i32` / `i64` / `f32` / `void` / `ptr[<元素类型>]` / `[<N> x <元素类型>]`。
指针**显式携带元素类型**（不用 LLVM 的 opaque `ptr`），见 S05 报告 §3.3。

**子 Region**：第一个 Region 的 `{` 跟在 Op 行尾；多 Region 的 Op（`IfOp`）
的后续 Region 各自用**单独一行**的 `{`；每个 Region 用**单独一行**的 `}`
在同缩进处收尾。

**缩进**：2 空格/层。**dump 是纯函数**：同一输入两次运行逐字节相同，
不含绝对路径/时间戳/随机数/指针地址。
