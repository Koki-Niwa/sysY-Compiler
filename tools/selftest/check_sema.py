#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_sema.py —— S03 的**三合一**独立验证器（对 490 个文件只扫一遍）

它一次负责三件事（phases/S03-sema.md §六/§七）：

  ① **轨 A**：范围内用例 `--emit=sema` 退出码 0 且 stderr 无 `error:`；
             范围外用例必须报诊断、退出码非 0、且不崩（信号/超时）。
  ② **§六 第 4 条 · 去注解还原**：把 `--emit=sema` 的文本按三步去掉注解后，
             必须与同一文件的 `--emit=ast` 输出**逐字节相同**。
  ③ **轨 B · 16 条类型不变式**：见 InvariantChecker 的注释。

格式层（去注释 / 还原器 / 类型工具）在 `sema_dump_format.py`，两者都不
import 编译器的任何代码。

── 为什么是流式的（prompt §九 的体积警告）────────────────────────────────
  `--emit=ast` 对 `86_long_code2.sy`（88 KB 源码）产出 **128.7 MB**；实测
  `--emit=sema` 是 **129.0 MB**。若把整份文本读成 Python 对象树，会吃掉几 GB。
  所以：还原器**边剥注解边喂 SHA-256**；不变式检查器**按行**推进（这个格式
  每行恰好一个节点头 + 末尾若干右括号），栈深度 = 树深，内存 O(树深)。
  两者在同一次逐行遍历里完成 —— 一份文本只读一遍。
  实测：那个 129 MB 的文件走完全流程 **1.73 s / 文件**（含两次编译进程）。

用法:
    check_sema.py --compiler <可执行文件> [--jobs N] [--root DIR] [--filter GLOB]
                  [--timeout SEC] [--verbose] [--json FILE] [--no-revert]
退出码：0 = 全过；1 = 有违反；2 = 工具自身错误。
"""

import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from sema_dump_format import (ARITH, REL, LOGIC, OPS, PLAIN, RT_ORDER, CAST_IN,
                              Reverter, out_of_scope_reason, parse_type,
                              ty_rank, ty_elem, ty_drop, ty_text, ty_scalar)

# ============================================================================
# 一、轨 B：16 条类型不变式（流式、按行）
#
#   格式不变式（这个格式每行恰好一个节点头，右括号只出现在行尾）：
#       <缩进>(<节点头记号...>          ← 新节点
#       ... 若干子节点行 ...
#       <缩进>(<节点头...>))))          ← 行尾的 k 个 ')' 关闭 k 个节点
#   所以按行解析即可：每行产生一个节点，再按行尾括号数依次"收账"。
# ============================================================================
class SExprNode:
    __slots__ = ('sym', 'name', 'vtype', 'objtype', 'kind', 'target',
                 'kids', 'line', 'is_const')

    def __init__(self, sym, line):
        self.sym = sym
        self.name = None
        self.vtype = None        # 值类型（表达式节点）
        self.objtype = None      # LVal 的 :obj
        self.kind = None         # Cast 的种类
        self.target = None       # Cast 的目标类型
        self.kids = []
        self.line = line
        self.is_const = False


class InvariantChecker:
    def __init__(self):
        self.stack = []
        self.violations = []      # [(rule#, 行号, 说明)]
        self.line_no = 0
        self.in_header = True
        self.header_depth = 0
        self.funcs = {}           # 名字 -> {'ret':t, 'params':[t], 'line':n}
        self.rt = {}              # 运行时函数 -> 同上
        self.scopes = []          # 作用域栈：名字 -> {'type':t,'const':bool,'global':bool}
        self.in_compunit = False
        self.cur_func = []        # FuncDef 的返回类型栈
        self.rt_seen = 0

    # ── 违规记录 ──────────────────────────────────────────────────────────
    def bad(self, rule, msg):
        if len(self.violations) < 200:
            self.violations.append((rule, self.line_no, msg))

    # ── RuntimeLib 头 ─────────────────────────────────────────────────────
    def _header(self, line):
        s = line.strip()
        m = re.match(r'^\(RuntimeFunc\s+(\w+)\s+(:[^\s)]+)(.*)$', s)
        if m:
            name, ret, rest = m.group(1), m.group(2)[1:], m.group(3)
            params = re.findall(r'\(Param\s+\w+\s+:t\s+([^\s)]+)', rest)
            uncallable = ':uncallable' in rest
            self.rt[name] = {'ret': ret, 'params': params, 'line': self.line_no,
                             'uncallable': uncallable}
            self.rt_seen += 1
        self.header_depth += line.count('(') - line.count(')')
        if self.header_depth == 0:
            self.in_header = False
            if self.rt_seen != len(RT_ORDER):
                self.bad(0, 'RuntimeLib 头里的运行时函数个数是 %d，期望 %d'
                         % (self.rt_seen, len(RT_ORDER)))
            elif list(self.rt.keys()) != RT_ORDER:
                self.bad(0, 'RuntimeLib 的打印顺序不是 sylib.h 的顺序：%s'
                         % ','.join(self.rt.keys()))
            # 名字 -> 签名（供 Call 检查用）
            self.funcs.update(self.rt)

    # ── 一行正文 ──────────────────────────────────────────────────────────
    def feed(self, line):
        self.line_no += 1
        if self.in_header:
            self._header(line)
            return
        s = line.strip()
        if not s:
            return
        i = s.find(')')
        head = s if i < 0 else s[:i]
        closes = 0 if i < 0 else s.count(')', i)
        toks = head.split()
        if not toks or not toks[0].startswith('('):
            self.bad(0, '不是合法的节点行：%r' % head[:60])
            return
        sym = toks[0][1:]
        node = SExprNode(sym, self.line_no)
        try:
            self._head(node, toks)
        except (IndexError, ValueError) as e:
            self.bad(0, '节点头解析失败：%r（%s）' % (head[:80], e))
            # ⚠️ **仍然入栈**：否则这一行的 `)` 会去关闭错误的节点，
            #    整个栈从此错位（S03 实测过：一个 `:?` 引出了几十条假违规）。
        self._open(node)
        for _ in range(closes):
            self._close()

    # ── 节点头（形状与 AstSexpLayout.h / SemaDump.cpp 一一对应）──────────
    def _head(self, node, t):
        sym = node.sym
        if sym in PLAIN:
            return
        if sym == 'Decl':
            if len(t) >= 3 and t[1] == ':const':
                node.is_const = True
                node.vtype = parse_type(t[2][1:])
            else:
                node.vtype = parse_type(t[1][1:])
            if node.vtype is None:
                raise ValueError('bad type')
            return
        if sym == 'FuncDef':
            node.name = t[1]
            node.vtype = parse_type(t[2][1:])
            return
        if sym in ('VarDef', 'Param'):
            node.name = t[1]
            if t[2] != ':t':
                raise ValueError('missing :t')
            node.vtype = parse_type(t[3])
            if node.vtype is None:
                raise ValueError('bad type')
            for extra in t[4:]:
                if extra.startswith(':'):
                    self.bad(1, '%s 的 :t 之后还有多余的类型记号 %s' % (sym, extra))
            return
        if sym in OPS:
            node.vtype = parse_type(t[1][1:])
            if node.vtype is None:
                raise ValueError('bad type')
            for extra in t[2:]:
                self.bad(1, '%s 之后还有多余的类型记号 %s' % (sym, extra))
            return
        if sym in ('IntLit', 'FloatLit'):
            node.name = t[1]
            node.vtype = parse_type(t[2][1:])
            if node.vtype is None:
                raise ValueError('bad type')
            want = 'int' if sym == 'IntLit' else 'float'
            if node.vtype[0] != want:
                self.bad(1, '%s 的值类型是 %s，应为 %s' % (sym, t[2], want))
            return
        if sym == 'Call':
            node.name = t[1]
            node.vtype = parse_type(t[2][1:])
            if node.vtype is None:
                raise ValueError('bad type')
            return
        if sym == 'LVal':
            node.name = t[1]
            if t[2] != ':obj':
                raise ValueError('missing :obj')
            node.objtype = parse_type(t[3])
            node.vtype = parse_type(t[4][1:])
            if node.objtype is None or node.vtype is None:
                raise ValueError('bad type')
            if len(t) > 5:
                self.bad(1, 'LVal 的 :obj 之后还有多余记号：%s' % ' '.join(t[5:]))
            return
        if sym == 'Cast':
            node.kind = t[1][1:]
            node.target = parse_type(t[2][1:])
            if node.target is None:
                raise ValueError('bad type')
            # ★ §五(d)：Cast 的值类型**由种类唯一决定**（不写第三个记号）
            derived = CAST_IN.get(node.kind)
            if derived is None:
                raise ValueError('unknown cast kind')
            node.vtype = parse_type(derived[0])
            return
        raise ValueError('unknown node %s' % sym)

    # ── 进入 / 关闭 ───────────────────────────────────────────────────────
    def _open(self, node):
        if node.sym == 'CompUnit':
            self.scopes.append({})
            self.in_compunit = True
        elif node.sym == 'Block':
            self.scopes.append({})
        elif node.sym == 'FuncDef':
            # 形参有自己的作用域（与函数体 Block 是两层 —— 与 Sema.cpp 一致）
            self.scopes.append({})
            self.cur_func.append(node.vtype)
            # 函数声明（在**进入**时登记，才支持递归）
            if self.scopes:
                self.scopes[0][node.name] = {'type': None, 'const': False,
                                             'global': True, 'func': node}
            self.funcs[node.name] = {'ret': ty_text(node.vtype), 'params': [],
                                     'uncallable': False}
        elif node.sym == 'params':
            pass
        elif node.sym == 'Param':
            self._declare(node.name, node.vtype, False, False)
        self.stack.append(node)

    def _declare(self, name, ty, is_const, is_global):
        if name is None:
            return
        self.scopes[-1][name] = {'type': ty, 'const': is_const,
                                 'global': is_global, 'func': None}

    def _lookup(self, name):
        for sc in reversed(self.scopes):
            if name in sc:
                return sc[name]
        return None

    def _close(self):
        if not self.stack:
            self.bad(0, '右括号多于左括号')
            return
        n = self.stack.pop()
        self._check(n)
        if self.stack:
            self.stack[-1].kids.append(n)
            self._check_child(n, self.stack[-1])
        if n.sym in ('Block', 'CompUnit'):
            if self.scopes:
                self.scopes.pop()
            if n.sym == 'CompUnit':
                self.in_compunit = False
        if n.sym == 'FuncDef':
            if self.cur_func:
                self.cur_func.pop()
            if self.scopes:
                self.scopes.pop()

    # ========================================================================
    # 逐节点的检查
    # ========================================================================
    def _check(self, n):
        s = n.sym
        k = n.kids
        # ── 运算符节点：先按"一元 / 二元"分流，再按运算符定规则号 ──────────
        if s in OPS:
            if s in ('+', '-', '!') and len(k) == 1:
                if s == '!':
                    if not _is_int(k[0].vtype):
                        self.bad(6, '! 的子节点类型是 %s，应为 int（float 要先 ToBool）'
                                 % ty_text(k[0].vtype))
                    if n.vtype != ('int', []):
                        self.bad(6, '! 的结果类型是 %s，应为 int' % ty_text(n.vtype))
                else:
                    if k[0].vtype != n.vtype:
                        self.bad(7, '一元 %s 的子类型 %s != 结果类型 %s'
                                 % (s, ty_text(k[0].vtype), ty_text(n.vtype)))
                    if not ty_scalar(n.vtype):
                        self.bad(7, '一元 %s 的结果类型 %s 不是标量'
                                 % (s, ty_text(n.vtype)))
            elif len(k) == 2:
                a, b = k[0].vtype, k[1].vtype
                if s in ARITH:
                    if a != b:
                        self.bad(2, '%s 的两个子节点类型不同：%s vs %s'
                                 % (s, ty_text(a), ty_text(b)))
                    elif a != n.vtype:
                        self.bad(2, '%s 的结果类型 %s 与子节点类型 %s 不同'
                                 % (s, ty_text(n.vtype), ty_text(a)))
                    if s == '%' and (not _is_int(a) or not _is_int(b)):
                        self.bad(3, '%% 的子节点类型必须是 int，实际 %s vs %s'
                                 % (ty_text(a), ty_text(b)))
                elif s in REL:
                    if a != b:
                        self.bad(4, '%s 的两个子节点类型不同：%s vs %s'
                                 % (s, ty_text(a), ty_text(b)))
                    if n.vtype != ('int', []):
                        self.bad(4, '%s 的结果类型是 %s，应为 int'
                                 % (s, ty_text(n.vtype)))
                else:                                   # && ||
                    for c in k:
                        if not _is_int(c.vtype):
                            self.bad(5, '%s 的子节点类型是 %s，应为 int'
                                     % (s, ty_text(c.vtype)))
                    if n.vtype != ('int', []):
                        self.bad(5, '%s 的结果类型是 %s，应为 int'
                                 % (s, ty_text(n.vtype)))
            else:
                r = 2 if s in ARITH else (4 if s in REL else 5)
                self.bad(r, '%s 的子节点个数是 %d，应为 2' % (s, len(k)))
        # ── 8：赋值 ───────────────────────────────────────────────────────
        elif s == '=':
            if len(k) != 2:
                self.bad(8, '= 的子节点个数是 %d，应为 2' % len(k))
            else:
                lhs, rhs = k
                if lhs.sym != 'LVal':
                    self.bad(8, '= 的左侧不是 LVal，而是 %s' % lhs.sym)
                else:
                    if not ty_scalar(lhs.vtype):
                        self.bad(8, '= 左侧的 LVal 没有完全下标（值类型 %s）'
                                 % ty_text(lhs.vtype))
                    d = self._lookup(lhs.name)
                    if d is not None and d.get('const'):
                        self.bad(8, '= 的左侧 %r 是 const 对象' % lhs.name)
                if lhs.vtype != rhs.vtype:
                    self.bad(8, '= 两侧类型不同：%s vs %s'
                             % (ty_text(lhs.vtype), ty_text(rhs.vtype)))
        # ── 9：return ─────────────────────────────────────────────────────
        elif s == 'Return':
            ret = self.cur_func[-1] if self.cur_func else None
            if k:
                if ret is not None and k[0].vtype != ret:
                    self.bad(9, 'return 的值类型 %s != 函数返回类型 %s'
                             % (ty_text(k[0].vtype), ty_text(ret)))
            else:
                if ret is not None and ret[0] != 'void':
                    self.bad(9, '非 void 函数的 return 没有值')
        # ── 10：Cast ──────────────────────────────────────────────────────
        elif s == 'Cast':
            want = CAST_IN.get(n.kind)
            if want is None:
                self.bad(10, '未知的 Cast 种类 %r' % n.kind)
            else:
                tgt, inp = want
                if ty_text(n.target) != tgt:
                    self.bad(10, 'Cast :%s 的目标类型是 %s，应为 %s'
                             % (n.kind, ty_text(n.target), tgt))
                if len(k) != 1:
                    self.bad(10, 'Cast 的子节点个数是 %d，应为 1' % len(k))
                elif ty_text(k[0].vtype) != inp:
                    self.bad(10, 'Cast :%s 的子节点类型是 %s，应为 %s'
                             % (n.kind, ty_text(k[0].vtype), inp))
        # ── 11：条件 ──────────────────────────────────────────────────────
        elif s in ('While', 'If'):
            if not k:
                self.bad(11, '%s 没有条件子节点' % s)
            elif not _is_int(k[0].vtype):
                self.bad(11, '%s 的条件类型是 %s，应为 int（float 要先 ToBool）'
                         % (s, ty_text(k[0].vtype)))
        # ── 12/13：Call ───────────────────────────────────────────────────
        elif s == 'Call':
            self._check_call(n)
        # ── 14：LVal ──────────────────────────────────────────────────────
        elif s == 'LVal':
            self._check_lval(n)
        # ── 15：VarDef / Param 的 :t ──────────────────────────────────────
        elif s in ('VarDef', 'Param'):
            r = ty_rank(n.vtype)
            ndim = sum(1 for c in k if c.sym == 'Dim')
            if r != ndim:
                self.bad(15, '%s %r 的 :t 是 %s（%d 维），但有 %d 个 Dim 子节点'
                         % (s, n.name, ty_text(n.vtype), r, ndim))
            if s == 'VarDef':
                if self.stack and self.stack[-1].sym == 'Decl':
                    base = self.stack[-1].vtype
                    if base is not None and base[0] != n.vtype[0]:
                        self.bad(15, 'VarDef %r 的 :t 基类型 %s 与所属 Decl 的 %s 不一致'
                                 % (n.name, n.vtype[0], base[0]))
            else:
                if r > 0 and n.vtype[1][0] is not None:
                    self.bad(15, '形参 %r 的第一维必须是未知的 []，实际 %s'
                             % (n.name, ty_text(n.vtype)))
        # ── 16：void 只允许"整条表达式语句的 void 调用" ────────────────────
        if s in EXPR_SYMS and n.vtype is not None and n.vtype[0] == 'void' and s != 'Call':
            self.bad(16, '%s 的值类型是 void（只有 Call 可能是 void）' % s)

    def _check_lval(self, n):
        if n.objtype[0] == 'void':
            self.bad(14, 'LVal %r 的 :obj 类型是 void' % n.name)
            return
        idx = len(n.kids)
        R = ty_rank(n.objtype)
        if n.vtype != ty_drop(n.objtype, idx):
            self.bad(14, 'LVal %r：:obj %s 去掉 %d 个下标后应为 %s，实际 %s'
                     % (n.name, ty_text(n.objtype), idx,
                        ty_text(ty_drop(n.objtype, idx)), ty_text(n.vtype)))
        if idx > R:
            self.bad(14, 'LVal %r：下标个数 %d > :obj 的维数 %d'
                     % (n.name, idx, R))
        elif idx < R:
            parent = self.stack[-1] if self.stack else None
            if parent is None or parent.sym != 'Call':
                self.bad(14, 'LVal %r 只给了 %d 个下标（:obj 是 %s），'
                             '但它的父节点不是 Call' % (n.name, idx, ty_text(n.objtype)))
        d = self._lookup(n.name)
        if d is not None and d.get('type') is not None and d['type'] != n.objtype:
            self.bad(15, 'LVal %r 的 :obj 是 %s，但声明处的类型是 %s'
                     % (n.name, ty_text(n.objtype), ty_text(d['type'])))

    def _check_call(self, n):
        sig = self.funcs.get(n.name)
        if sig is None:
            self.bad(12, '调用 %r 找不到对应的 FuncDef/RuntimeFunc' % n.name)
            return
        if sig.get('uncallable'):
            self.bad(12, '调用了不可调用的运行时函数 %r' % n.name)
            return
        params = [parse_type(p) for p in sig['params']]
        if len(n.kids) != len(params):
            self.bad(12, '%s 的实参个数 %d != 形参个数 %d'
                     % (n.name, len(n.kids), len(params)))
            return
        if sig['ret'] != ty_text(n.vtype):
            self.bad(12, '%s 的 Call 值类型是 %s，函数返回类型是 %s'
                     % (n.name, ty_text(n.vtype), sig['ret']))
        for arg, want in zip(n.kids, params):
            got = arg.vtype
            if want is None or got is None:
                continue
            if ty_rank(want) == 0:
                if got != want:
                    self.bad(13, '%s 的标量实参类型 %s != 形参类型 %s'
                             % (n.name, ty_text(got), ty_text(want)))
                continue
            if ty_rank(got) == 0:
                self.bad(13, '%s 的实参是标量 %s，形参是数组 %s'
                         % (n.name, ty_text(got), ty_text(want)))
                continue
            if ty_elem(got) != ty_elem(want):
                self.bad(13, '%s 的数组实参元素类型 %s != 形参元素类型 %s'
                         % (n.name, ty_elem(got), ty_elem(want)))
                continue
            if ty_rank(want) == 1:
                pass                       # 秩 1 的 T[] 接受任意秩的 T 数组
            elif ty_rank(got) != ty_rank(want) or ty_drop(got, 1) != ty_drop(want, 1):
                self.bad(13, '%s 的数组实参剩余维 %s 与形参 %s 不匹配'
                         % (n.name, ty_text(got), ty_text(want)))
            # 15 的后半：全局 const 对象不得作为**可写**的数组实参
            if arg.sym == 'LVal' and ty_rank(want) >= 1:
                d = self._lookup(arg.name)
                if d is not None and d.get('const'):
                    self.bad(15, 'const 对象 %r 被当作可写数组实参传给 %s'
                             % (arg.name, n.name))

    # ── 父子关系相关的检查（在子节点挂到父节点时做）───────────────────────
    def _check_child(self, child, parent):
        # 16：void 只允许作为**表达式语句**的整个表达式
        # ⚠️ 只对**表达式**节点判：`void` 函数定义的 FuncDef 也带 `:void`
        #    （那是返回类型，不是值类型）。漏了这句会把每个 void 函数报一次。
        if (child.sym in EXPR_SYMS and child.vtype is not None
                and child.vtype[0] == 'void'):
            if parent.sym != 'ExprStmt':
                self.bad(16, 'void 调用出现在 %s 内部（只允许作为整条表达式语句）'
                         % parent.sym)
        # 形参表登记到函数签名（供 12/13 用）
        if child.sym == 'params' and parent.sym == 'FuncDef':
            self.funcs[parent.name] = {
                'ret': ty_text(parent.vtype),
                'params': [ty_text(p.vtype) for p in child.kids],
                'uncallable': False}
        # 声明登记（VarDef 只能在这里登记：它的初始化器要先解析完）
        if child.sym == 'Decl':
            is_global = parent.sym == 'CompUnit'
            for d in child.kids:
                if d.sym == 'VarDef':
                    self._declare(d.name, d.vtype, child.is_const, is_global)

    def finish(self):
        if self.stack:
            self.bad(0, '有 %d 个节点没有闭合（括号不配平）' % len(self.stack))


EXPR_SYMS = OPS | {'IntLit', 'FloatLit', 'LVal', 'Call', 'Cast'}


def _is_int(t):
    return t is not None and t[0] == 'int' and len(t[1]) == 0


# ============================================================================
# 二、逐文件流程
# ============================================================================
def _run(cmd, timeout):
    return subprocess.run(cmd, capture_output=True, timeout=timeout)


def check_one(compiler, rel, root, workdir, timeout, do_revert):
    res = {'path': rel, 'status': 'fail', 'detail': '', 'violations': [],
           'seconds': 0.0, 'reverted': False}
    sy = os.path.join(root, rel)
    try:
        with open(sy, 'rb') as f:
            text = f.read().decode('utf-8', 'replace')
    except OSError as e:
        res['status'] = 'error'
        res['detail'] = '无法读取源码: %s' % e
        return res

    reason = out_of_scope_reason(text)
    tag = rel.replace('/', '__')
    sema_path = os.path.join(workdir, tag + '.sema')
    ast_path = os.path.join(workdir, tag + '.ast')
    t0 = time.time()
    try:
        p = subprocess.run([compiler, sy, '--emit=sema', '-o', sema_path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        res['status'] = 'error'
        res['detail'] = '超时（>%ss）' % timeout
        res['seconds'] = time.time() - t0
        return res
    err = p.stderr.decode('utf-8', 'replace')

    if reason is not None:
        # 范围外：必须报诊断、退出码非 0、不崩
        if p.returncode == 0:
            res['detail'] = '范围外文件（%s）却退出 0' % reason
        elif 'error:' not in err:
            res['detail'] = '范围外文件（%s）退出 %d 但 stderr 没有 error:' % (
                reason, p.returncode)
        elif p.returncode < 0 or p.returncode > 1:
            res['detail'] = '范围外文件退出码异常 %d（崩溃？）' % p.returncode
        else:
            res['status'] = 'skipped'
        return res

    # ── 范围内：轨 A ──────────────────────────────────────────────────────
    if p.returncode != 0:
        res['detail'] = '退出码 %d；stderr 前几行：\n%s' % (
            p.returncode, '\n'.join(err.strip().split('\n')[:6]))
        return res
    if 'error:' in err:
        res['detail'] = 'stderr 里有 error:：\n%s' % '\n'.join(
            err.strip().split('\n')[:6])
        return res

    # ── §六4 去注解还原 + 轨 B：**一份文本只读一遍** ──────────────────────
    if do_revert:
        pa = subprocess.run([compiler, sy, '--emit=ast', '-o', ast_path],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            timeout=timeout)
        if pa.returncode != 0:
            res['detail'] = '--emit=ast 退出码 %d' % pa.returncode
            return res
        h_ast = hashlib.sha256()
        with open(ast_path, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h_ast.update(chunk)
    else:
        h_ast = None

    rev = Reverter()
    chk = InvariantChecker()
    h_rev = hashlib.sha256()
    try:
        with open(sema_path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                out = rev.feed(line)
                if out:
                    h_rev.update(out.encode('utf-8'))
                chk.feed(line)
        tail = rev.finish()
        if tail:
            h_rev.update(tail.encode('utf-8'))
        chk.finish()
    except ValueError as e:
        res['status'] = 'error'
        res['detail'] = '还原器/检查器异常：%s' % e
        return res

    if do_revert:
        res['reverted'] = h_rev.hexdigest() == h_ast.hexdigest()
        if not res['reverted']:
            res['detail'] = ('去注解还原后与 --emit=ast 不同（sha256 %s vs %s）'
                             % (h_rev.hexdigest()[:16], h_ast.hexdigest()[:16]))
    if chk.violations:
        res['violations'] = chk.violations
        if not res['detail']:
            r, ln, msg = chk.violations[0]
            res['detail'] = '轨 B 不变式 #%d 违规（第 %d 行）：%s' % (r, ln, msg)
    res['status'] = 'ok' if (not chk.violations and
                             (res['reverted'] or not do_revert)) else 'fail'
    res['seconds'] = time.time() - t0
    return res


# ============================================================================
# 三、主流程
# ============================================================================
def discover(root):
    out = []
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in ('.git', '__pycache__')]
        for f in fn:
            if f.endswith('.sy'):
                out.append(os.path.relpath(os.path.join(dp, f), root))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description='S03 三合一验证器（轨 A + 去注解还原 + 轨 B）')
    ap.add_argument('--compiler', required=True, help='编译器可执行文件路径')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--root', default='/home/koki1/try/tests')
    ap.add_argument('--filter', default=None, help='只跑匹配该 glob 的相对路径')
    ap.add_argument('--timeout', type=float, default=300.0)
    ap.add_argument('--json', default=None)
    ap.add_argument('--no-revert', action='store_true', help='跳过去注解还原（调试用）')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    files = discover(args.root)
    if args.filter:
        files = [f for f in files if glob.fnmatch.fnmatch(f, args.filter)]
    if not files:
        print('没有匹配的用例（--root / --filter 对吗？）', file=sys.stderr)
        return 2

    t0 = time.time()
    results = []
    with tempfile.TemporaryDirectory(prefix='sema_check_') as wd:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(check_one, compiler, f, args.root, wd, args.timeout,
                              not args.no_revert): f for f in files}
            for fut in concurrent.futures.as_completed(futs):
                results.append(fut.result())
    dt = time.time() - t0
    results.sort(key=lambda r: r['path'])

    ok = [r for r in results if r['status'] == 'ok']
    skipped = [r for r in results if r['status'] == 'skipped']
    bad = [r for r in results if r['status'] in ('fail', 'error')]

    print('=' * 78)
    print('  统计')
    print('    范围内通过 (ok)        : %d' % len(ok))
    print('    范围外已报诊断 (skipped): %d' % len(skipped))
    print('    失败         (fail)    : %d' % len(bad))
    print('    合计                   : %d' % len(results))
    print('    耗时                   : %.2f s（并行度 %d）' % (dt, args.jobs))
    if results:
        slow = sorted(results, key=lambda r: -r['seconds'])[:3]
        print('    最慢三个               : %s' % ', '.join(
            '%s %.2fs' % (os.path.basename(r['path']), r['seconds']) for r in slow))
    print('=' * 78)

    rules = {}
    for r in results:
        for rule, _ln, _msg in r['violations']:
            rules[rule] = rules.get(rule, 0) + 1
    for r in bad:
        print('✘ %s' % r['path'])
        if r['detail']:
            for ln in r['detail'].split('\n'):
                print('    %s' % ln)
        for rule, lineno, msg in r['violations'][:6]:
            print('    [轨B #%d] 第 %d 行：%s' % (rule, lineno, msg))
        if len(r['violations']) > 6:
            print('    … 还有 %d 条' % (len(r['violations']) - 6))
    if rules:
        print('  违规按不变式编号统计：%s' % ', '.join(
            '#%d×%d' % (k, v) for k, v in sorted(rules.items())))

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({'seconds': dt, 'ok': len(ok), 'skipped': len(skipped),
                       'fail': len(bad), 'results': results}, f,
                      ensure_ascii=False, indent=1)

    if bad:
        print('判定：✘ 有 %d 个用例未通过' % len(bad))
        return 1
    print('判定：✔ 轨 A（490 零误报）+ 去注解还原 + 轨 B（16 条不变式）全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
