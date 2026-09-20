/* probe.c —— 链路自检用的【手写 C 文件】（P00 prompt §4.7）
 *
 * 它不依赖我们的编译器：用 clang -S -emit-llvm 生成一个"假的编译器输出"，
 * 再喂给 run_tests.sh 的 ②→⑤ 步（清 IR → llvm-as → llc → 链接 → 运行 → 比对）。
 *
 * 自检通过标准：输入 21 → stdout "42\n"，退出码 7，
 * 按 C0 规则预期完整输出 "42\n7\n"（见 TESTING-GUIDE §2）。
 */
#include <stdio.h>

int main(void) {
    int n;
    if (scanf("%d", &n) != 1) return 1;
    printf("%d\n", n * 2);
    return 7;
}
