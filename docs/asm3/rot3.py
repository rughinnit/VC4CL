#!/usr/bin/env python3
"""Primitive probe with a control row.  Rows written (VPM rows 0..n-1), one DMA:
   0: f32(999) constant            (control: proves fresh data)
   1: v = lane+1
   2: rep_all(v)
   3: rep_all(rot(v,1))
   4: rep_all(rot(v,2))
   5: rep_all(rot(v,15))
   6: rot(v,1)
   7: rot(v,2)
   8: rot(v,15)
   9: v8min(v,v) no imm (identity)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qenc import *          # noqa: F401,F403
from qenc import _ops
from gen import Prog

MUL_V8MIN = 4


def rot(out, src, n):
    return _ops(mul_op=MUL_V8MIN, mul_out=32 + out, ws=0, mul_a=acc(src), mul_b=imm(48 + n),
                add_op=ADD_NOP, cond_add=COND_NEVER)


def rep(src, ws=1):
    return _ops(add_op=ADD_OR, add_out=37, ws=ws, add_a=acc(src), add_b=acc(src))


def build():
    p = Prog()
    p.emit(_ops(add_op=ADD_OR, add_out=19, add_a=areg(R_UNIF), add_b=areg(R_UNIF)))  # u0
    p.emit(_ops(add_op=ADD_OR, add_out=16, add_a=areg(R_UNIF), add_b=areg(R_UNIF)))  # u1
    p.emit(ldi(0, 18))                                   # ra18 = 0  (VPM row 0)
    p.emit(_ops(add_op=ADD_OR, add_out=20, add_a=areg(R_ELEM_NUM), add_b=areg(R_ELEM_NUM)))
    p.emit(nop())
    p.emit(_ops(add_op=ADD_ITOF, add_out=32, add_a=areg(20), add_b=areg(20)))   # acc0 = lane
    p.emit(ldi(0x3F800000, 21, ws=1))
    p.emit(nop(), nop())
    p.emit(_ops(add_op=ADD_FADD, add_out=32, add_a=acc(0), add_b=(REGB, 21)))   # acc0 = lane+1
    p.emit(ldi(0x1A00, R5, ws=1))
    p.emit(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1, add_a=acc(5), add_b=areg(18)))
    # 0: control constant 999.0
    p.emit(ldi(0x4479C000, 17))                          # ra17 = 999.0f
    p.emit(nop(), nop())
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=areg(17), add_b=areg(17)))
    # 1: v
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=areg(0), add_b=areg(0)))
    # 2: rep(v)
    p.emit(rep(0))
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=acc(5), add_b=acc(5)))
    # 3,4,5: rep(rot(v,n))
    for n in (1, 2, 15):
        p.emit(rot(1, 0, n))
        p.emit(rep(1))
        p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=acc(5), add_b=acc(5)))
    # 6,7,8: rot(v,n)
    for n in (1, 2, 15):
        p.emit(rot(1, 0, n))
        p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=acc(1), add_b=acc(1)))
    # 9: identity v8min with no small immediate
    p.emit(_ops(mul_op=MUL_V8MIN, mul_out=33, ws=0, mul_a=acc(0), mul_b=acc(0),
                add_op=ADD_NOP, cond_add=COND_NEVER))
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=acc(1), add_b=acc(1)))
    # store 10 rows
    p.emit(ldi((2 << 30) | (10 << 23) | (16 << 16) | (1 << 14), R5, ws=1))
    p.emit(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1, add_a=acc(5), add_b=areg(18)))
    p.emit(ldi(0xC0010000, W_VPW_SETUP, ws=1))
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPW_ADDR, ws=1, add_a=areg(16), add_b=areg(16)))
    p.emit(_ops(add_op=ADD_OR, add_out=W_NOP,
                add_a=(REGB, R_VPW_WAIT), add_b=(REGB, R_VPW_WAIT)))
    p.emit(end_program())
    p.emit(nop(), nop(), nop())
    return p


if __name__ == '__main__':
    ASM = os.path.dirname(os.path.abspath(__file__))
    build().write(os.path.join(ASM, 'rot3.bin'))
