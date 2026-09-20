#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
independent_parser_check.py -- 独立交叉验证器（SysY2022 语法分析器 + AST 打印器）

本脚本**不复用**被测编译器的任何代码：它自己实现
    1) 一个 SysY2022 词法分析器（依据 docs/sysy_lang.txt 第 2 节 Terminal Symbols）
    2) 一个递归下降 / 优先级爬升语法分析器（依据 docs/sysy_lang.txt 第 2 节 EBNF）
    3) 一个 S-表达式打印器
然后把生成的文本与被测编译器 `--emit=ast` 的输出做**逐字节**比较。

用法:
    python3 independent_parser_check.py --compiler /path/to/compiler [--filter GLOB]
                                        [--jobs N] [--verbose] [--keep-tmp]
                                        [--root DIR] [--dump FILE]

退出码:
    0 = 全部一致
    1 = 存在不一致
    2 = 工具自身错误（如编译器不可用、命令行错误）

零第三方依赖：只用 Python3 标准库。

独立性说明：本文件在写成之前没有阅读 compiler/src/frontend/Parser.{h,cpp}、
compiler/src/frontend/Ast.h、compiler/tools/selftest/{unit/test_parser.cpp,check_parser.py}，
也没有阅读 /home/koki1/try/.reference/。文法、词法规则全部来自 docs/sysy_lang.txt；
打印格式通过 `--emit=ast` 的实际输出观察确定。
"""

import argparse
import fnmatch
import os
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

# --------------------------------------------------------------------------
# 0. 常量
# --------------------------------------------------------------------------

KEYWORDS = frozenset((
    'const', 'int', 'float', 'void',
    'if', 'else', 'while', 'break', 'continue', 'return',
))

# 两字符运算符必须先于单字符匹配
PUNCT2 = ('==', '!=', '<=', '>=', '&&', '||')
PUNCT1 = tuple('+-*/%=<>!()[]{};,')

WS = ' \t\r\n\v\f'


class ParseError(Exception):
    """词法/语法错误。msg 是纯文本诊断，pos 为 (line, col)。"""

    def __init__(self, msg, line=0, col=0):
        super().__init__(msg)
        self.msg = msg
        self.line = line
        self.col = col

    def __str__(self):
        return '%d:%d: %s' % (self.line, self.col, self.msg)


# --------------------------------------------------------------------------
# 1. 词法分析器
# --------------------------------------------------------------------------

class Token(object):
    __slots__ = ('kind', 'text', 'line', 'col')

    def __init__(self, kind, text, line, col):
        self.kind = kind    # 'id' | 'kw' | 'int' | 'float' | 'punct' | 'eof'
        self.text = text
        self.line = line
        self.col = col

    def __repr__(self):
        return 'Token(%s,%r,%d,%d)' % (self.kind, self.text, self.line, self.col)


def _is_digit(c):
    return '0' <= c <= '9'


def _is_hex(c):
    return ('0' <= c <= '9') or ('a' <= c <= 'f') or ('A' <= c <= 'F')


def _is_id_start(c):
    return ('a' <= c <= 'z') or ('A' <= c <= 'Z') or c == '_'


def _is_id_cont(c):
    return _is_id_start(c) or _is_digit(c)


def tokenize(src):
    """把源码变成 token 列表（末尾恒有一个 'eof'）。词法错误直接抛 ParseError。"""
    toks = []
    i = 0
    n = len(src)
    line = 1
    linestart = 0

    def col(pos):
        return pos - linestart + 1

    while i < n:
        c = src[i]
        # --- 空白 ---
        if c in WS:
            if c == '\n':
                line += 1
                linestart = i + 1
            i += 1
            continue
        # --- 注释 ---
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i + 2)
            if j < 0:
                i = n
            else:
                i = j            # 换行留给空白分支处理（行号递增）
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            if j < 0:
                raise ParseError('unterminated block comment', line, col(i))
            chunk = src[i:j + 2]
            nl = chunk.count('\n')
            if nl:
                line += nl
                linestart = i + chunk.rfind('\n') + 1
            i = j + 2
            continue
        # --- 标识符 / 关键字 ---
        if _is_id_start(c):
            j = i + 1
            while j < n and _is_id_cont(src[j]):
                j += 1
            text = src[i:j]
            toks.append(Token('kw' if text in KEYWORDS else 'id', text, line, col(i)))
            i = j
            continue
        # --- 数字常量 ---
        if _is_digit(c) or (c == '.' and i + 1 < n and _is_digit(src[i + 1])):
            tok, i, line, linestart = _lex_number(src, i, line, linestart)
            toks.append(tok)
            continue
        # --- 运算符 / 分隔符 ---
        two = src[i:i + 2]
        if two in PUNCT2:
            toks.append(Token('punct', two, line, col(i)))
            i += 2
            continue
        if c in PUNCT1:
            toks.append(Token('punct', c, line, col(i)))
            i += 1
            continue
        raise ParseError('invalid character %r (0x%02x)' % (c, ord(c)), line, col(i))

    toks.append(Token('eof', '<eof>', line, n - linestart + 1))
    return toks


def _lex_number(src, i, line, linestart):
    """词法：整数常量(decimal/octal/hex) 与 浮点常量。

    浮点常量按 C99 floating-constant：fractional-constant[exponent] | digit-seq exponent。
    所有后缀一律忽略（本实现也不接受后缀，遇到 f/F/l/L 会留给后续 token 化）。
    返回 (Token, new_i, new_line, new_linestart)。
    """
    n = len(src)
    start = i
    col = i - linestart + 1

    # 十六进制整数 / 十六进制浮点常量（C99 hexadecimal-floating-constant）
    if src[i] == '0' and i + 1 < n and src[i + 1] in 'xX':
        j = i + 2
        intdigits = 0
        while j < n and _is_hex(src[j]):
            j += 1
            intdigits += 1
        hexfloat = False
        fracdigits = 0
        if j < n and src[j] == '.':
            # hexadecimal-fractional-constant
            hexfloat = True
            j += 1
            while j < n and _is_hex(src[j]):
                j += 1
                fracdigits += 1
            if intdigits == 0 and fracdigits == 0:
                raise ParseError('invalid hexadecimal constant', line, col)
        elif intdigits == 0:
            raise ParseError('invalid hexadecimal constant', line, col)
        # binary-exponent-part: 十六进制浮点必须带 p/P 指数
        if j < n and src[j] in 'pP':
            k = j + 1
            if k < n and src[k] in '+-':
                k += 1
            if k < n and _is_digit(src[k]):
                while k < n and _is_digit(src[k]):
                    k += 1
                j = k
                hexfloat = True
            elif hexfloat:
                raise ParseError('missing binary exponent in hexadecimal '
                                 'floating constant', line, col)
        elif hexfloat:
            raise ParseError('missing binary exponent in hexadecimal '
                             'floating constant', line, col)
        return (Token('float' if hexfloat else 'int', src[start:j], line, col),
                j, line, linestart)

    is_float = False
    j = i
    while j < n and _is_digit(src[j]):
        j += 1
    # 小数点
    if j < n and src[j] == '.':
        is_float = True
        j += 1
        while j < n and _is_digit(src[j]):
            j += 1
    elif src[i] == '.':
        # 以 '.' 开头（前面 while 没吃掉任何数字）
        is_float = True
        j += 1
        while j < n and _is_digit(src[j]):
            j += 1
    # 指数部分
    if j < n and src[j] in 'eE':
        k = j + 1
        if k < n and src[k] in '+-':
            k += 1
        if k < n and _is_digit(src[k]):
            while k < n and _is_digit(src[k]):
                k += 1
            j = k
            is_float = True
    text = src[start:j]

    if not is_float:
        # 八进制检查：C 里 08/09 非法
        if len(text) > 1 and text[0] == '0':
            for ch in text[1:]:
                if ch in '89':
                    raise ParseError('invalid digit %r in octal constant' % ch,
                                     line, col)
    return Token('float' if is_float else 'int', text, line, col), j, line, linestart


# --------------------------------------------------------------------------
# 2. 语法树
# --------------------------------------------------------------------------

class Node(object):
    """head 是 '(Name' 后面紧跟的那一段（含内联的原子，如 'VarDef g'），
    kids 是子节点列表。打印时：kids 为空 -> '(head)'；否则 '(head' 换行缩进子节点，
    最后一个子节点行尾补 ')'。"""

    __slots__ = ('head', 'kids')

    def __init__(self, head, kids=None):
        self.head = head
        self.kids = kids if kids is not None else []


def render(root):
    """把语法树渲染成文本（含结尾换行）。使用显式栈，避免深递归。"""
    lines = []
    stack = [('n', root, 0)]
    while stack:
        item = stack.pop()
        if item[0] == 'c':
            lines[-1] += ')'
            continue
        _, node, ind = item
        pref = ' ' * ind + '(' + node.head
        if not node.kids:
            lines.append(pref + ')')
        else:
            lines.append(pref)
            stack.append(('c',))
            for k in reversed(node.kids):
                stack.append(('n', k, ind + 2))
    return '\n'.join(lines) + '\n'


# --------------------------------------------------------------------------
# 3. 语法分析器（递归下降 + 优先级爬升）
# --------------------------------------------------------------------------

class Parser(object):
    def __init__(self, toks):
        self.toks = toks
        self.p = 0

    # ---- 基础工具 ----
    def peek(self, k=0):
        j = self.p + k
        if j >= len(self.toks):
            return self.toks[-1]
        return self.toks[j]

    def at(self, text):
        t = self.peek()
        return (t.kind in ('punct', 'kw')) and t.text == text

    def at_kind(self, kind):
        return self.peek().kind == kind

    def next(self):
        t = self.toks[self.p]
        if t.kind != 'eof':
            self.p += 1
        return t

    def expect(self, text):
        t = self.peek()
        if (t.kind in ('punct', 'kw')) and t.text == text:
            return self.next()
        raise ParseError("expected '%s' but got %r" % (text, t.text), t.line, t.col)

    def expect_id(self):
        t = self.peek()
        if t.kind != 'id':
            raise ParseError('expected an identifier but got %r' % t.text, t.line, t.col)
        return self.next()

    # ---- CompUnit ----
    def parse_compunit(self):
        kids = []
        while not self.at_kind('eof'):
            kids.append(self.parse_toplevel())
        return Node('CompUnit', kids)

    def parse_toplevel(self):
        t = self.peek()
        if t.text == 'const':
            return self.parse_const_decl()
        if t.text in ('int', 'float'):
            # BType Ident ... -> '(' 则为函数定义，否则是变量声明
            if self.peek(1).kind == 'id' and self.peek(2).text == '(':
                return self.parse_funcdef()
            return self.parse_var_decl()
        if t.text == 'void':
            return self.parse_funcdef()
        raise ParseError('expected a declaration or function definition but got %r'
                         % t.text, t.line, t.col)

    # ---- 声明 ----
    def parse_const_decl(self):
        self.expect('const')
        btype = self.parse_btype()
        defs = [self.parse_const_def()]
        while self.at(','):
            self.next()
            defs.append(self.parse_const_def())
        self.expect(';')
        return Node('Decl :const :' + btype, defs)

    def parse_var_decl(self):
        btype = self.parse_btype()
        defs = [self.parse_var_def()]
        while self.at(','):
            self.next()
            defs.append(self.parse_var_def())
        self.expect(';')
        return Node('Decl :' + btype, defs)

    def parse_btype(self):
        t = self.peek()
        if t.text in ('int', 'float') and t.kind == 'kw':
            self.next()
            return t.text
        raise ParseError("expected 'int' or 'float' but got %r" % t.text, t.line, t.col)

    def parse_const_def(self):
        name = self.expect_id().text
        dims = self.parse_dims(const=True)
        self.expect('=')
        init = self.parse_initval(const=True)
        return Node('VarDef ' + name, dims + [init])

    def parse_var_def(self):
        name = self.expect_id().text
        dims = self.parse_dims(const=True)
        kids = list(dims)
        if self.at('='):
            self.next()
            kids.append(self.parse_initval(const=False))
        return Node('VarDef ' + name, kids)

    def parse_dims(self, const):
        """声明里的 { '[' ConstExp ']' }；返回 Dim 节点列表。"""
        dims = []
        while self.at('['):
            self.next()
            e = self.parse_exp()
            self.expect(']')
            dims.append(Node('Dim', [e]))
        return dims

    def parse_initval(self, const):
        if self.at('{'):
            self.next()
            kids = []
            if not self.at('}'):
                kids.append(self.parse_initval(const))
                while self.at(','):
                    self.next()
                    kids.append(self.parse_initval(const))
            self.expect('}')
            return Node('InitVal', kids)
        e = self.parse_exp()
        return Node('InitVal', [e])

    # ---- 函数 ----
    def parse_funcdef(self):
        t = self.peek()
        if t.text not in ('void', 'int', 'float'):
            raise ParseError('expected a function return type but got %r' % t.text,
                             t.line, t.col)
        self.next()
        rtype = t.text
        name = self.expect_id().text
        self.expect('(')
        params = []
        if not self.at(')'):
            params.append(self.parse_funcfparam())
            while self.at(','):
                self.next()
                params.append(self.parse_funcfparam())
        self.expect(')')
        body = self.parse_block()
        return Node('FuncDef %s :%s' % (name, rtype), [Node('params', params), body])

    def parse_funcfparam(self):
        btype = self.parse_btype()
        del btype
        name = self.expect_id().text
        dims = []
        if self.at('['):
            self.next()
            self.expect(']')             # 第一维必须为空
            dims.append(Node('Dim'))
            while self.at('['):
                self.next()
                e = self.parse_exp()
                self.expect(']')
                dims.append(Node('Dim', [e]))
        return Node('Param ' + name, dims)

    # ---- 语句 ----
    def parse_block(self):
        self.expect('{')
        items = []
        while not self.at('}'):
            if self.at_kind('eof'):
                t = self.peek()
                raise ParseError("expected '}' but got end of file", t.line, t.col)
            items.append(self.parse_blockitem())
        self.expect('}')
        return Node('Block', items)

    def parse_blockitem(self):
        t = self.peek()
        if t.text == 'const':
            return self.parse_const_decl()
        if t.text in ('int', 'float') and t.kind == 'kw':
            # 块内不可能出现函数定义，'int'/'float' 必为声明
            return self.parse_var_decl()
        return self.parse_stmt()

    def parse_stmt(self):
        t = self.peek()
        if t.text == '{':
            return self.parse_block()
        if t.text == 'if':
            self.next()
            self.expect('(')
            cond = self.parse_cond()
            self.expect(')')
            then = self.parse_stmt()
            kids = [cond, then]
            if self.at('else'):
                self.next()
                kids.append(Node('Else', [self.parse_stmt()]))
            return Node('If', kids)
        if t.text == 'while':
            self.next()
            self.expect('(')
            cond = self.parse_cond()
            self.expect(')')
            body = self.parse_stmt()
            return Node('While', [cond, body])
        if t.text == 'break':
            self.next()
            self.expect(';')
            return Node('Break')
        if t.text == 'continue':
            self.next()
            self.expect(';')
            return Node('Continue')
        if t.text == 'return':
            self.next()
            if self.at(';'):
                self.next()
                return Node('Return')
            e = self.parse_exp()
            self.expect(';')
            return Node('Return', [e])
        if t.text == ';':
            self.next()
            return Node('ExprStmt')
        # LVal '=' Exp ';'  |  [Exp] ';'
        if t.kind == 'id':
            save = self.p
            try:
                lval = self.parse_lval()
            except ParseError:
                lval = None
                self.p = save
            if lval is not None and self.at('='):
                self.next()
                rhs = self.parse_exp()
                self.expect(';')
                return Node('=', [lval, rhs])
            self.p = save
        e = self.parse_exp()
        self.expect(';')
        return Node('ExprStmt', [e])

    # ---- 表达式 ----
    def parse_exp(self):
        """Exp -> AddExp"""
        return self.parse_add()

    def parse_cond(self):
        """Cond -> LOrExp"""
        return self.parse_lor()

    def parse_add(self):
        node = self.parse_mul()
        while True:
            t = self.peek()
            if t.kind == 'punct' and t.text in ('+', '-'):
                self.next()
                rhs = self.parse_mul()
                node = Node(t.text, [node, rhs])
            else:
                return node

    def parse_mul(self):
        node = self.parse_unary()
        while True:
            t = self.peek()
            if t.kind == 'punct' and t.text in ('*', '/', '%'):
                self.next()
                rhs = self.parse_unary()
                node = Node(t.text, [node, rhs])
            else:
                return node

    def parse_unary(self):
        t = self.peek()
        if t.kind == 'punct' and t.text in ('+', '-', '!'):
            self.next()
            return Node(t.text, [self.parse_unary()])
        return self.parse_primary()

    def parse_primary(self):
        t = self.peek()
        if t.kind == 'punct' and t.text == '(':
            self.next()
            e = self.parse_exp()
            self.expect(')')
            return e
        if t.kind == 'int':
            self.next()
            return Node('IntLit ' + t.text)
        if t.kind == 'float':
            self.next()
            return Node('FloatLit ' + t.text)
        if t.kind == 'id':
            if self.peek(1).kind == 'punct' and self.peek(1).text == '(':
                return self.parse_call()
            return self.parse_lval()
        raise ParseError('expected an expression but got %r' % t.text, t.line, t.col)

    def parse_lval(self):
        t = self.expect_id()
        kids = []
        while self.at('['):
            self.next()
            e = self.parse_exp()
            self.expect(']')
            kids.append(e)
        return Node('LVal ' + t.text, kids)

    def parse_call(self):
        t = self.expect_id()
        self.expect('(')
        args = []
        if not self.at(')'):
            args.append(self.parse_exp())
            while self.at(','):
                self.next()
                args.append(self.parse_exp())
        self.expect(')')
        return Node('Call ' + t.text, args)

    # ---- Cond 的四个层次 ----
    def parse_lor(self):
        node = self.parse_land()
        while self.at('||'):
            self.next()
            node = Node('||', [node, self.parse_land()])
        return node

    def parse_land(self):
        node = self.parse_eq()
        while self.at('&&'):
            self.next()
            node = Node('&&', [node, self.parse_eq()])
        return node

    def parse_eq(self):
        node = self.parse_rel()
        while True:
            t = self.peek()
            if t.kind == 'punct' and t.text in ('==', '!='):
                self.next()
                node = Node(t.text, [node, self.parse_rel()])
            else:
                return node

    def parse_rel(self):
        node = self.parse_add()
        while True:
            t = self.peek()
            if t.kind == 'punct' and t.text in ('<', '>', '<=', '>='):
                self.next()
                node = Node(t.text, [node, self.parse_add()])
            else:
                return node


def parse_source(src):
    """源码文本 -> AST 文本（与 --emit=ast 期望的格式一致）。"""
    toks = tokenize(src)
    return render(Parser(toks).parse_compunit())


def read_source(path):
    with open(path, 'r', encoding='utf-8', errors='surrogateescape') as f:
        return f.read()


def parse_file(path):
    return parse_source(read_source(path))


# --------------------------------------------------------------------------
# 4. 驱动器
# --------------------------------------------------------------------------

def workspace_root():
    here = os.path.dirname(os.path.abspath(__file__))
    # <root>/compiler/tools/selftest/this.py
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))


def discover(root, pattern_filter):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if not name.endswith('.sy'):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, workspace_root()).replace(os.sep, '/')
            if pattern_filter:
                if not (fnmatch.fnmatch(rel, pattern_filter)
                        or fnmatch.fnmatch(name, pattern_filter)):
                    continue
            out.append(full)
    out.sort()
    return out


class Result(object):
    __slots__ = ('path', 'rel', 'status', 'cpp_rc', 'detail', 'cpp_text',
                 'my_text', 'cpp_err', 'my_err')

    def __init__(self, path, rel):
        self.path = path
        self.rel = rel
        self.status = 'ok'
        self.cpp_rc = None
        self.detail = ''
        self.cpp_text = None
        self.my_text = None
        self.cpp_err = ''
        self.my_err = ''


def run_cpp(compiler, src, outpath, timeout):
    try:
        pr = subprocess.run([compiler, '--emit=ast', '-o', outpath, src],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, b'', b'timeout'
    return pr.returncode, pr.stdout, pr.stderr


def check_one(compiler, path, tmpdir, timeout):
    rel = os.path.relpath(path, workspace_root()).replace(os.sep, '/')
    res = Result(path, rel)
    outpath = os.path.join(tmpdir, rel.replace('/', '__') + '.ast')
    rc, _out, err = run_cpp(compiler, path, outpath, timeout)
    res.cpp_err = (err or b'').decode('utf-8', 'replace')
    if rc is None:
        res.status = 'cpp-timeout'
        res.detail = '被测编译器超时'
        return res
    res.cpp_rc = rc

    try:
        mine = parse_file(path)
        res.my_text = mine
        my_ok = True
    except ParseError as ex:
        res.my_err = str(ex)
        my_ok = False
    except RecursionError:
        res.my_err = 'recursion limit exceeded'
        my_ok = False

    if rc < 0:
        # 被信号杀死 = 崩溃
        res.status = 'cpp-crash'
        res.detail = '被测编译器被信号 %d 终止（崩溃）' % (-rc)
        return res
    if rc not in (0, 1):
        res.status = 'cpp-badrc'
        res.detail = '被测编译器退出码为 %d（既非 0 也非 1）' % rc
        return res

    if rc == 0:
        try:
            with open(outpath, 'rb') as f:
                cpp_bytes = f.read()
        except OSError as ex:
            res.status = 'tool-error'
            res.detail = '无法读取 --emit=ast 产物: %s' % ex
            return res
        res.cpp_text = cpp_bytes.decode('utf-8', 'surrogateescape')
        if not my_ok:
            res.status = 'parser-rejected'
            res.detail = 'C++ 接受但独立 parser 拒绝：%s' % res.my_err
            return res
        if res.my_text.encode('utf-8', 'surrogateescape') != cpp_bytes:
            res.status = 'mismatch'
            res.detail = 'AST 文本不一致'
            return res
        res.status = 'ok'
        return res

    # rc == 1：应当是被拒绝的程序
    if my_ok:
        res.status = 'cpp-rejected'
        res.detail = ('C++ 以退出码 1 拒绝，但独立 parser 接受（范围外语法或 C++ 过严）'
                      '；C++ stderr 首行: %s'
                      % (res.cpp_err.strip().splitlines() or ['<空>'])[0])
        return res
    res.status = 'rejected'
    return res


def first_diff(a, b, ctx=3, maxblocks=1):
    """返回 (lineno, a_lines_slice, b_lines_slice)，行号从 1 开始，原始文本。"""
    al = a.split('\n')
    bl = b.split('\n')
    n = max(len(al), len(bl))
    i = 0
    blocks = []
    while i < n and len(blocks) < maxblocks:
        if (al[i] if i < len(al) else None) != (bl[i] if i < len(bl) else None):
            lo = max(0, i - ctx)
            hi = min(n, i + ctx + 1)
            blocks.append((i + 1,
                           [(k + 1, al[k] if k < len(al) else '<缺行>') for k in range(lo, hi)],
                           [(k + 1, bl[k] if k < len(bl) else '<缺行>') for k in range(lo, hi)]))
            i = hi
        else:
            i += 1
    return blocks


def show_raw_diff(res, max_lines=24):
    """逐条打印原始差异片段：左边 C++，右边独立 parser。"""
    blocks = first_diff(res.cpp_text or '', res.my_text or '')
    if not blocks:
        print('    （两边文本相同？请检查编码/长度。cpp=%d bytes, mine=%d bytes）'
              % (len(res.cpp_text or ''), len(res.my_text or '')))
        return
    for lineno, al, bl in blocks:
        print('    首个差异行: %d' % lineno)
        print('    --- C++ --emit=ast 原文 ---')
        for k, text in al[:max_lines]:
            print('    %5d| %s' % (k, text))
        print('    --- 独立 parser 原文 ---')
        for k, text in bl[:max_lines]:
            print('    %5d| %s' % (k, text))
        print('    -------------------------')


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='SysY2022 语法分析器 + AST 打印器 独立交叉验证器')
    ap.add_argument('--compiler', default=None,
                    help='被测编译器可执行文件路径（默认 <root>/compiler/build/compiler）')
    ap.add_argument('--filter', default=None,
                    help='只跑匹配该 GLOB 的用例（匹配相对工作区路径或文件名）')
    ap.add_argument('--jobs', type=int, default=8, help='并行度，默认 8')
    ap.add_argument('--verbose', action='store_true', help='逐文件打印结果')
    ap.add_argument('--root', default=None, help='语料根目录（默认 <root>/tests）')
    ap.add_argument('--keep-tmp', action='store_true', help='保留临时输出目录')
    ap.add_argument('--timeout', type=float, default=60.0, help='单个文件超时秒数')
    ap.add_argument('--dump', default=None,
                    help='只对单个 .sy 文件打印独立 parser 的 AST 文本后退出')
    args = ap.parse_args(argv)

    # 深递归保护：语料里存在 ~4000 层的表达式树；--dump 路径同样需要
    sys.setrecursionlimit(200000)
    for stack_bytes in (256 * 1024 * 1024, 64 * 1024 * 1024, 16 * 1024 * 1024):
        try:
            threading.stack_size(stack_bytes)
            break
        except (ValueError, RuntimeError):
            continue

    ws = workspace_root()
    if args.dump:
        try:
            sys.stdout.write(parse_file(args.dump))
        except ParseError as ex:
            sys.stderr.write('REJECT %s: %s\n' % (args.dump, ex))
            return 1
        return 0

    compiler = args.compiler or os.path.join(ws, 'compiler', 'build', 'compiler')
    if not os.path.isfile(compiler):
        sys.stderr.write('工具错误: 找不到被测编译器 %s\n' % compiler)
        return 2
    if not os.access(compiler, os.X_OK):
        sys.stderr.write('工具错误: %s 不可执行\n' % compiler)
        return 2
    if args.jobs < 1:
        sys.stderr.write('工具错误: --jobs 必须 >= 1\n')
        return 2

    testroot = args.root or os.path.join(ws, 'tests')
    if not os.path.isdir(testroot):
        sys.stderr.write('工具错误: 找不到语料目录 %s\n' % testroot)
        return 2

    files = discover(testroot, args.filter)
    if not files:
        sys.stderr.write('工具错误: --filter %r 没有匹配到任何 .sy 文件\n' % args.filter)
        return 2

    tmpdir = os.path.join(ws, '.indep_parser_check_tmp')
    if os.path.isdir(tmpdir):
        shutil.rmtree(tmpdir)
    os.makedirs(tmpdir)

    # 深递归保护已在 main() 开头设置
    print('== SysY2022 Parser/AST 独立交叉验证 ==')
    print('工作区       : %s' % ws)
    print('被测编译器   : %s' % compiler)
    print('语料根目录   : %s' % testroot)
    print('用例数       : %d  (.sy 文件%s)'
          % (len(files), ('，filter=%s' % args.filter) if args.filter else ''))
    print('并行度       : %d' % args.jobs)
    print('比较方式     : 未做任何 strip/规范化的整文件逐字节比较；')
    print('               即 len(mine) == len(cpp) 且每一个字节都相同，'
          '含缩进空格、行尾与结尾换行。')
    print('')

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(check_one, compiler, f, tmpdir, args.timeout) for f in files]
        for i, fu in enumerate(futs):
            res = fu.result()
            results.append(res)
            if args.verbose:
                print('  [%4d/%4d] %-12s %s' % (i + 1, len(files), res.status, res.rel))
                if res.status not in ('ok', 'rejected'):
                    print('              %s' % res.detail)

    counts = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1

    n_ok = counts.get('ok', 0)
    n_rej = counts.get('rejected', 0)
    bad = [r for r in results if r.status not in ('ok', 'rejected')]

    print('-- 结果统计 --')
    print('  逐字节一致                 : %d' % n_ok)
    print('  双方都拒绝（范围外/非法）  : %d' % n_rej)
    print('  不一致/异常                : %d' % len(bad))
    for k in sorted(counts):
        if k not in ('ok', 'rejected'):
            print('    - %-16s : %d' % (k, counts[k]))
    # 拒绝类用例的退出码分布
    rej_rc = {}
    for r in results:
        if r.status == 'rejected':
            rej_rc[r.cpp_rc] = rej_rc.get(r.cpp_rc, 0) + 1
    print('  被拒绝用例的 C++ 退出码分布: %s'
          % ', '.join('%s->%d 个' % (k, v) for k, v in sorted(rej_rc.items())))
    print('  崩溃(信号终止)用例数       : %d'
          % sum(1 for r in results if r.status == 'cpp-crash'))
    print('')

    if bad:
        print('-- 不一致明细（最多列出前 3 条，原文照抄） --')
        for idx, r in enumerate(bad[:3], 1):
            print('[%d] %s' % (idx, r.rel))
            print('    状态: %s' % r.status)
            print('    说明: %s' % r.detail)
            if r.status == 'mismatch':
                show_raw_diff(r)
            else:
                if r.my_err:
                    print('    独立 parser 诊断: %s' % r.my_err)
                if r.cpp_err.strip():
                    print('    C++ stderr:')
                    for line in r.cpp_err.strip().splitlines()[:6]:
                        print('      %s' % line)
            print('')
        if len(bad) > 3:
            print('  （另有 %d 条未列出）' % (len(bad) - 3))
            print('  全部不一致用例清单:')
            for r in bad:
                print('    %-12s %s' % (r.status, r.rel))
        print('')

    if not args.keep_tmp:
        shutil.rmtree(tmpdir, ignore_errors=True)
    else:
        print('临时输出保留在: %s' % tmpdir)

    if bad:
        print('结论: 存在 %d 条不一致 -> 退出码 1' % len(bad))
        return 1
    print('结论: %d 个用例逐字节一致，%d 个用例双方一致拒绝 -> 退出码 0'
          % (n_ok, n_rej))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(2)
