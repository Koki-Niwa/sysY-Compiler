#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iic_front —— `independent_irgen_check` 的**源码侧前端层**。

单一职责（拆出来是为了满足 §C4 的 800 行上限）：
  ① 词法器（`tokenize`）+ 由 Sema 树引导的**行号标注器**（`Annotator`）——
     独立实现拿不到 Sema 转储里的行列号（那份转储不含 loc），只能回读源文件贴回去；
  ② `--emit=initplan` 的解析（`Act`/`Entry`/`mk_local`/`parse_initplan`）。
本层**只依赖** `iic_sexp`（S-表达式与 IR 类型），不依赖 IRGen 与驱动。
"""
import re

from iic_sexp import Node, sexpr, subs, one, parse_ty    # noqa: F401

# ==================== 二、源码词法 + 行号标注器 ====================
_TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)|(?P<lc>//[^\n]*)|(?P<bc>/\*.*?\*/)
  | (?P<fl>0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)[pP][+-]?\d+
            |(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)
  | (?P<il>0[xX][0-9a-fA-F]+|0[0-7]*|[1-9]\d*)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>==|!=|<=|>=|&&|\|\||[-+*/%<>=!()\[\]{},;])
""", re.X | re.S)
_KEYWORDS = {'int', 'float', 'void', 'const', 'if', 'else', 'while', 'break', 'continue', 'return'}
BINOPS = {'+', '-', '*', '/', '%', '<', '>', '<=', '>=', '==', '!=', '&&', '||'}
_KIND = {'fl': 'FLOAT', 'il': 'INT', 'op': 'OP'}
class Tok(object):
    __slots__ = ('kind', 'text', 'line')
    def __init__(self, kind, text, line):
        self.kind, self.text, self.line = kind, text, line
def tokenize(src):
    toks, i, line = [], 0, 1
    while i < len(src):
        m = _TOKEN_RE.match(src, i)
        if not m:
            raise ValueError('lex error at offset %d' % i)
        i, kind, text = m.end(), m.lastgroup, m.group()
        if kind in ('ws', 'lc', 'bc'):
            line += text.count('\n')
        elif kind == 'id':
            toks.append(Tok('KW' if text in _KEYWORDS else 'ID', text, line))
        else:
            toks.append(Tok(_KIND[kind], text, line))
    return toks
class Annotator(object):
    """按 Sema 树的结构消费源码 token，把行号写到 Sema 节点上（node.line）。
    分组括号在 Sema 树里是透明的：当它与当前期望的 token 不符时直接跳过。
    """
    def __init__(self, toks):
        self.t, self.i = toks, 0
    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None
    def take(self, text=None, kind=None):
        while (self.peek() is not None and self.peek().text in '()' and self.peek().text != text):
            self.i += 1
        tk = self.peek()
        if tk is None or (text is not None and tk.text != text) or (kind and tk.kind != kind):
            raise ValueError('line %s: want %r/%s got %r' % ( tk.line if tk else 'EOF', text, kind, tk.text if tk else 'EOF'))
        self.i += 1; return tk
    def dims(self, n):
        for d in [c for c in subs(n) if c[0] == 'Dim']:
            self.take('[')
            if one(d) is not None:
                self.expr(one(d))
            self.take(']')
    def decl(self, n):
        if self.peek() and self.peek().text == 'const':
            self.take('const')
        self.take(kind='KW')                                   # BType
        for k, vd in enumerate([c for c in subs(n) if c[0] == 'VarDef']):
            if k:
                self.take(',')
            self.vardef(vd)
        self.take(';')
    def vardef(self, n):
        n.line = self.take(kind='ID').line                     # 声明符所在行
        self.dims(n); iv = [c for c in subs(n) if c[0] == 'InitVal']
        if iv:
            self.take('='); self.initval(iv[0])
    def initval(self, n):
        n.line = self.peek().line
        if self.peek().text == '{':
            self.take('{')
            for k, c in enumerate(subs(n)):
                if k:
                    self.take(',')
                self.initval(c) if c[0] == 'InitVal' else self.expr(c)
            self.take('}')
        elif subs(n):
            self.expr(subs(n)[0])
    def funcdef(self, n):
        n.line = self.take(kind='KW').line                     # 返回类型所在行
        self.take(kind='ID'); self.take('('); ps = [c for c in subs(n) if c[0] == 'params']
        for k, p in enumerate(subs(ps[0]) if ps else []):
            if k:
                self.take(',')
            p.line = self.take(kind='KW').line; self.take(kind='ID'); self.dims(p)
        self.take(')'); self.block([c for c in subs(n) if c[0] == 'Block'][0])
    def block(self, n):
        n.line = self.take('{').line
        for c in subs(n):
            self.stmt(c)
        self.take('}')
    def stmt(self, n):
        h = n[0]
        if h == 'Block':
            self.block(n)
        elif h == 'Decl':
            self.decl(n)
        elif h == '=':
            a, b = subs(n); self.expr(a); self.take('=')
            n.line = a.line                                    # Store 用左值行
            self.expr(b); self.take(';')
        elif h == 'ExprStmt':
            if subs(n):                            # 空语句 `;` → 无子节点
                self.expr(subs(n)[0])
            if self.peek() and self.peek().text == ';':
                self.take(';')
        elif h == 'If':
            n.line = self.take('if').line; self.take('('); self.expr(subs(n)[0]); self.take(')'); self.stmt(subs(n)[1])
            el = [c for c in subs(n) if c[0] == 'Else']
            if el:
                self.take('else'); self.stmt(subs(el[0])[0])
        elif h == 'While':
            n.line = self.take('while').line; self.take('('); self.expr(subs(n)[0]); self.take(')'); self.stmt(subs(n)[1])
        elif h == 'Return':
            n.line = self.take('return').line
            if subs(n):
                self.expr(subs(n)[0])
            self.take(';')
        else:                                                  # Break / Continue
            n.line = self.take(h.lower()).line; self.take(';')
    def expr(self, n):
        h = n[0]
        if h in ('IntLit', 'FloatLit'):
            n.line = self.take(kind='INT' if h == 'IntLit' else 'FLOAT').line
        elif h == 'LVal':
            n.line = self.take(kind='ID').line
            for c in subs(n):
                self.take('['); self.expr(c); self.take(']')
        elif h == 'Call':
            n.line = self.take(kind='ID').line; self.take('(')
            for k, c in enumerate(subs(n)):
                if k:
                    self.take(',')
                self.expr(c)
            self.take(')')
        elif h == 'Cast':
            self.expr(subs(n)[0]); n.line = subs(n)[0].line     # Cast 对位置透明
        elif h in BINOPS or h == '!':
            k = subs(n)
            if len(k) == 1:                                    # 一元：前缀运算符
                n.line = self.take(h).line; self.expr(k[0])
            else:                                              # 二元：中缀，运算符行
                self.expr(k[0]); n.line = self.take(h).line; self.expr(k[1])
        else:
            raise ValueError('expr: unexpected %s' % h)
# ============================== 三、InitPlan ==============================
class Act(object):
    __slots__ = ('kind', 'off', 'ty', 'val', 'expr')
    def __init__(self, kind, off, ty=None, val=None, expr=None):
        self.kind, self.off, self.ty, self.val, self.expr = kind, off, ty, val, expr
class Entry(object):
    __slots__ = ('name', 'ty', 'zero', 'data', 'acts')
    def __init__(self, name, ty):
        self.name, self.ty, self.zero, self.data, self.acts = name, ty, False, [], []
def mk_local(loc):
    """一个 (Local <func>/<name> :t <ty> :actions <act>...) 节点 → Entry。"""
    e = Entry(loc[1].split('/', 1)[1], parse_ty(loc[3]))
    for a in subs(loc):
        if a[0] == 'Zero':
            e.acts.append(Act('Zero', int(a[1]), val=int(a[2])))
        elif a[0] == 'StoreConst':
            e.acts.append(Act('StoreConst', int(a[1]), a[2], a[3]))
        elif a[0] == 'StoreExpr':
            e.acts.append(Act('StoreExpr', int(a[1]), a[2], expr=subs(a)[0]))
        else:                                                  # MemcpyConst
            e.acts.append(Act('MemcpyConst', int(a[1]), a[2], val=[x for x in a[3:] if not isinstance(x, list)]))
    return e
def parse_initplan(text):
    """→ (globals, {func: [Entry]})，保持 S04 的顺序。
    ⚠ S04 的 InitPlan 转储**括号不平衡**：每个 `(Func ...)` 的右括号都没打印，
    只有最后一个 Func 由结尾的 `)` 收掉。这里按 dump 缩进（顶层项 2 空格）切项再
    各自配平——只补括号，不改内容。
    """
    lines = text.split('\n'); starts = [i for i, l in enumerate(lines) if re.match(r'^  \(', l)]
    globs, funcs = [], {}
    for k, st in enumerate(starts):
        en = starts[k + 1] if k + 1 < len(starts) else len(lines); txt = '\n'.join(l for l in lines[st:en] if not re.match(r'^\)', l))
        c = sexpr(txt + ')' * (txt.count('(') - txt.count(')')))[0]
        if c[0] == 'Global':
            e = Entry(c[1], parse_ty(c[3]))
            if ':zero' in c[1:]:
                e.zero = True
            else:
                e.data = [(int(p[0]), p[1]) for p in subs(c)]
            globs.append(e)
        elif c[0] == 'Func':
            funcs[c[1]] = [mk_local(loc) for loc in subs(c)]
    return globs, funcs
