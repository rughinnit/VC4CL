#!/usr/bin/env python3
"""VPM->DRAM store prober.

The store path is copied instruction-for-instruction from vc4c --asm of
src/probe_vstore4.cl:

    or ra0, qpu_num, qpu_num
    ldi rep_all|r5, 0x1A34            ; generic VPM write setup, row 52
    add vpw_setup, r5, ra0            ; -> row 52+qpu, 32-bit packed horiz, stride 1
    or vpm, rb0, rb0                  ; 16 words into that VPM row
    shl rb0, ra0, 7
    ldi rep_all|r5, 0x80845A00        ; VDW DMA setup: row 52+qpu, horiz, depth 4 words
    add vpw_setup, r5, rb0
    ldi vpw_setup, 0xC0010000         ; VDW stride setup: memory stride 0
    or vpw_addr, rb1, rb1             ; trigger the DMA to that address
    or -, vpw_wait, vpw_wait          ; wait for completion

Only the DMA setup word (depth/units) is varied here, so the "depth 1 word"
hypothesis for the old wedge can be tested directly.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qenc import *          # noqa: F401,F403
from qenc import _ops
from gen import Prog


def dma_word(depth, units, row=52):
    assert 1 <= depth <= 127 and 1 <= units <= 127
    return (2 << 30) | (units << 23) | (depth << 16) | (1 << 14) | (row << 7)


DMA_VC4C = 0x80845A00            # depth 4 words (16 B), units 1  <- vc4c probe
DMA_OLD = 0x80815A00             # depth 1 word  (4 B), units 1  <- old failing const


def store_seq(p, qpu_a, val_a, addr_a, word, wait=True):
    """vc4c store sequence. qpu_a/val_a/addr_a are A-file registers written
    early enough (2 instruction slots) by the caller."""
    p.emit(ldi(0x1A34, R5, ws=1))                                   # rep_all|r5
    p.emit(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1,
                add_a=acc(5), add_b=areg(qpu_a)))
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPM, add_a=areg(val_a), add_b=areg(val_a)))
    p.emit(_ops(add_op=ADD_SHL, add_out=0, ws=1, add_a=areg(qpu_a), add_b=imm(7)))
    p.emit(ldi(word, R5, ws=1))
    p.emit(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1,
                add_a=acc(5), add_b=(REGB, 0)))
    p.emit(ldi(0xC0010000, W_VPW_SETUP, ws=1))
    p.emit(_ops(add_op=ADD_OR, add_out=W_VPW_ADDR, ws=1,
                add_a=areg(addr_a), add_b=areg(addr_a)))
    if wait:
        p.emit(_ops(add_op=ADD_OR, add_out=W_NOP,
                    add_a=(REGB, R_VPW_WAIT), add_b=(REGB, R_VPW_WAIT)))


def build_store(word, val='lane', nstore=1, wait=True):
    """out[qpu][0..15] = (float)lane, stored once per call."""
    p = Prog()
    p.emit(_ops(add_op=ADD_OR, add_out=6, add_a=areg(R_UNIF), add_b=areg(R_UNIF)))  # u0 niter
    p.emit(_ops(add_op=ADD_OR, add_out=7, add_a=areg(R_UNIF), add_b=areg(R_UNIF)))  # u1 out bus addr
    p.emit(_ops(add_op=ADD_OR, add_out=5, add_a=breg(R_QPU_NUM), add_b=breg(R_QPU_NUM)))
    p.emit(_ops(add_op=ADD_OR, add_out=2, add_a=areg(R_ELEM_NUM), add_b=areg(R_ELEM_NUM)))
    p.emit(nop())
    if val == 'lane':
        p.emit(_ops(add_op=ADD_ITOF, add_out=0, add_a=areg(2), add_b=areg(2)))
        p.emit(nop())
        p.emit(nop())
    elif val == 'one':
        p.emit(ldi(0x3F800000, 0))
        p.emit(nop(), nop())
    for i in range(nstore):
        if i:
            p.emit(ldi(64, 21, ws=1))
            p.emit(_ops(add_op=ADD_ADD, add_out=7, add_a=areg(7), add_b=(REGB, 21)))
            p.emit(nop(), nop())
        store_seq(p, 5, 0, 7, word, wait=wait)
    p.emit(end_program())
    p.emit(nop(), nop(), nop())
    return p


if __name__ == '__main__':
    outdir = os.path.dirname(os.path.abspath(__file__))
    p = build_store(DMA_VC4C)
    w = p.write(os.path.join(outdir, 'store_ok.bin'))
    print('store_ok.bin', len(w), 'instructions')
    print(listing(w))
