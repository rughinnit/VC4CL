#!/usr/bin/env python3
"""Multi-row VPM->DRAM store (the GEMM store primitive).

Writes `nrows` per-lane vectors into nrows consecutive VPM rows with a single
generic write setup (stride 1 row, so each `or vpm` advances one row), then
issues ONE VPM DMA with depth=16 words, units=nrows rows and a programmable
memory stride between rows.

Row base is nrows*qpu, so 12 QPUs x 4 rows = rows 0..47, inside the 64-row VPM.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qenc import *          # noqa: F401,F403
from qenc import _ops
from gen import Prog


def dma_word(depth, units, row):
    return (2 << 30) | ((units % 128) << 23) | ((depth % 128) << 16) | (1 << 14) | ((row & 0x3F) << 7)


def build_rows(nrows=4, stride=0, nbase=0x41800000):
    """out[qpu][r*16+lane] = 16*r + lane"""
    p = Prog()
    p.emit(_ops(add_op=ADD_OR, add_out=7, add_a=areg(R_UNIF), add_b=areg(R_UNIF)))  # u0 niter
    p.emit(_ops(add_op=ADD_OR, add_out=9, add_a=areg(R_UNIF), add_b=areg(R_UNIF)))  # u1 out bus addr
    p.emit(_ops(add_op=ADD_OR, add_out=5, add_a=breg(R_QPU_NUM), add_b=breg(R_QPU_NUM)))
    p.emit(nop(), nop())
    sh = nrows.bit_length() - 1
    assert (1 << sh) == nrows, 'nrows must be a power of two'
    p.emit(_ops(add_op=ADD_SHL, add_out=6, add_a=areg(5), add_b=imm(sh)))
    p.emit(nop(), nop())                                            # ra6 = qpu*nrows
    p.emit(_ops(add_op=ADD_SHL, add_out=10, ws=1, add_a=areg(6), add_b=imm(7)))
    p.emit(nop())                                                   # rb10 = qpu*nrows*128
    p.emit(_ops(add_op=ADD_OR, add_out=2, add_a=areg(R_ELEM_NUM), add_b=areg(R_ELEM_NUM)))
    p.emit(nop())
    p.emit(_ops(add_op=ADD_ITOF, add_out=0, add_a=areg(2), add_b=areg(2)))
    p.emit(nop(), nop())                                            # ra0 = (float)lane
    p.emit(ldi(nbase, 20, ws=1))                                    # 16.0f
    # generic write setup once: address = nrows*qpu, 32-bit, packed, horiz, stride 1
    p.emit(ldi(0x1A00, R5, ws=1))
    p.emit(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1, add_a=acc(5), add_b=areg(6)))
    # nrows vector writes, each advances one VPM row
    for r in range(nrows):
        if r:
            p.emit(_ops(add_op=ADD_FADD, add_out=0, add_a=areg(0), add_b=(REGB, 20)))
            p.emit(nop(), nop())
        p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=areg(0), add_b=areg(0)))
    # DMA setup: depth 16 words, units nrows, row base nrows*qpu
    p.emit(ldi(dma_word(16, nrows, 0), R5, ws=1))
    p.emit(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1,
                add_a=acc(5), add_b=(REGB, 10)))
    p.emit(ldi(0xC0010000 | (stride & 0xFFFF), W_VPW_SETUP, ws=1))
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPW_ADDR, ws=1, add_a=areg(9), add_b=areg(9)))
    p.emit(_ops(add_op=ADD_OR, add_out=W_NOP,
                add_a=(REGB, R_VPW_WAIT), add_b=(REGB, R_VPW_WAIT)))
    p.emit(end_program())
    p.emit(nop(), nop(), nop())
    return p


if __name__ == '__main__':
    outdir = os.path.dirname(os.path.abspath(__file__))
    p = build_rows(4)
    w = p.write(os.path.join(outdir, 'rows4.bin'))
    print('rows4.bin', len(w), 'instructions')
    print(listing(w))
