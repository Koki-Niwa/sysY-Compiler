# `flat/example/` —— `--emit=flat-ir` 的格式契约

`example.emit-flat.txt` 是编译器对 `example.sy` 的真实输出。C8 关卡会重新生成
它，并用 `cmp -s` 做逐字节比较；修改打印格式时必须用真实产物更新这份文件。

这个小例子固定了全局常量、函数与基本块的顺序，也覆盖了由 `For` 的归纳变量
生成的 `phi`、条件分支、用户函数调用、运行时 `void` 调用、具名常量和源码行号。

格式要点如下：

```text
@name = global <type> { <offset>=<value>, ... } @line <n>
define <ret> @name(<type> %arg, ...) {
  %n = <type> <constant> @line <n>
  %n = ptr[<type>] @global
L0:
  %n = <opcode> ... @line <n>
  %n = phi <type> [(%value Lpred), ...] @line <n>
  br i1 %cond, label Ltrue, label Lfalse @line <n>
}
```

参数先编号；其余结果按基本块和指令顺序编号。常量与全局地址在函数的第一个
标签前打印，常量按编号排序。标签顶格，指令缩进两个空格。`phi` 必须位于块首，
且每个入值都显式带前驱标签。源位置存在时放在行尾；合成的值可以没有
`@line`。同一输入的输出必须稳定，并且经 `--from-flat` 读回后逐字节不变。
