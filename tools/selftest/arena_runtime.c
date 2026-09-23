/* arena_runtime.c —— `arena.py` 的 **gcc 侧真值运行库**。
 *
 * ⚠️⚠️ 这里的读入语义必须与 `compiler/tools/selftest/exec_core.py` 的
 *      `Machine.next_int` / `getch` / `getarray` / `next_float` **逐条对齐**，否则 gcc
 *      就不是独立真值（那整套三方对照就白做了）：
 *
 *        getint()    → 第 k 个 PRNG 值（stdin 上按顺序给的整数）
 *        getch()     → 65 + (第 k 个 PRNG 值 & 15)
 *        getarray(a) → **恒 8 个** PRNG 值写进 a，返回 8
 *        getfloat() / getfarray(a) → 依次消耗整数，Python 式 % 1000 再除以 4
 *
 *      `arena.py` 用**同一个** `gen_inputs()` 生成 stdin，所以两边看到的
 *      整数序列逐字节相同。
 *
 * 只提供 SysY 运行时里"有语义"的那几个；计时函数是空实现（轨迹里不含时间）。
 */
#include <stdio.h>

static int read_int(void) {
    int v;
    if (scanf("%d", &v) != 1) return 0;
    return v;
}

int getint(void) { return read_int(); }

int getch(void) { return 65 + (read_int() & 15); }

int getarray(int a[]) {
    int n = 8, i;                    /* ★ 恒 8，与 exec_core 一致 */
    for (i = 0; i < n; i++) a[i] = read_int();
    return n;
}

static float read_float(void) {
    int v = read_int() % 1000;
    if (v < 0) v += 1000;             /* Python 的 %，负整数仍给出非负余数 */
    return (float)v / 4.0f;
}

float getfloat(void) { return read_float(); }

int getfarray(float a[]) {
    int n = 8, i;
    for (i = 0; i < n; i++) a[i] = read_float();
    return n;
}

void putint(int x) { printf("%d", x); }
void putch(int c) { putchar(c); }
void putfloat(float x) { printf("%a", (double)x); }
void putarray(int n, int a[]) {
    int i;
    printf("%d:", n);
    for (i = 0; i < n; i++) printf(" %d", a[i]);
    putchar('\n');
}
void putfarray(int n, float a[]) {
    int i;
    printf("%d:", n);
    for (i = 0; i < n; i++) printf(" %a", (double)a[i]);
    putchar('\n');
}
void starttime(void) {}
void stoptime(void) {}
