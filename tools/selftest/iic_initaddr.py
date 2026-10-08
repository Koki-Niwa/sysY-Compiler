#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""独立 IRGen 的 InitPlan 对象形状及字节偏移寻址。"""
from iic_shapes import obj_of
from iic_sexp import elem_of, n_leaf


def init_address(gen, ptr, vty, offset, line):
    """沿完整数组对象逐维寻址，不将多维数组展平成 i32 指针。"""
    gp, curty, leaf = ptr, vty, offset // 4
    for _ in range(max(1, len(vty[1]))):
        inner = elem_of(curty) if curty[1] else curty
        index, leaf = divmod(leaf, n_leaf(inner))
        idx = gen.sext(gen.ci(index, line)[0], line)
        T = obj_of(inner)
        gp = gen.val('GetElementPtr @ %s i64 0 %s %s'
                     % (T, gp, idx), line, 'ptr[%s]' % T, None)[0]
        curty = inner
    return gp
