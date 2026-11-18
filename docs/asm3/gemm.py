#!/usr/bin/env python3
"""Hand-written VideoCore IV QPU GEMM: 16x16 C tile per QPU, 16 lanes = 16 columns.

Verified data path (primitive probes, see NOTES.md):
  * TMU gather: lane r receives A[rowblk+r][k]     (per-lane stride N*4)
  * TMU gather: lane c receives B[k][colblk+c]     (per-lane stride 4)
  * broadcast A[r][k] to all 16 lanes in 2 instructions:
        v8min acc1, acc0, acc0 rot(16-r)   (mul ALU small-immediate vector rotate)
        or    rep_all|r5, acc1, acc1       (hardware element-0 replicate port)
  * MAC (1 word): fmul acc2, r5, r4 ; fadd C[r], C[r], acc2
    (the product reads the *previous* broadcast while this word's rep writes the
     next one: ALU reads precede writes, same register -> old value)
  * store: 4 chunks x 4 rows, one VPM DMA each, units=4 depth=16 stride=(N-16)*4,
    VPM rows 4*qpu..4*qpu+3

Per-k body: 39 instructions (5 head + 33 MAC/broadcast + 1 branch; the three
branch delay slots carry A15, the A-address bump and the B-address bump).
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qenc import *          # noqa: F401,F403
from qenc import _ops
from gen import Prog

MUL_V8MIN = 4
NQPU = 12

# ---- register map -----------------------------------------------------------
# A file: ra0..ra15 = C rows; addresses and counters below
RC = 0
U_N, U_C, U_B, U_A = 16, 17, 18, 19      # uniforms
RL, T, VPMQ = 20, 22, 25                 # lane, tile index, qpu*4 (VPM row base)
U4, TSTR_A = 21, 23                      # spare uniform, tile-stride source
AADR, BADR, CADR = 26, 27, 28            # gather/store addresses
SA, KCNT = 30, 31                        # tile scratch, k counter
# B file
BST, NEG1, NEGNT, VPMQ7 = 20, 21, 22, 23  # N*4, -1, -ntiles, qpu*4*128
COLOFF, ROWOFF, ALANE, BLANE = 25, 26, 27, 28
TSTR = 29                                # tiles-per-QPU stride
CSTEP = 24


def simm(v):
    assert 0 <= v <= 15, "small immediate out of range: %d" % v
    return imm(v)


def mul_fmul(out_acc, a, b):
    return dict(mul_op=MUL_FMUL, mul_out=32 + out_acc, ws=0, mul_a=a, mul_b=b)


def mul_rot(out_acc, src_acc, n):
    return dict(mul_op=MUL_V8MIN, mul_out=32 + out_acc, ws=0,
                mul_a=acc(src_acc), mul_b=imm(48 + n))


def add_or(out, a, b, ws=0):
    return dict(add_op=ADD_OR, add_out=out, ws=ws, add_a=a, add_b=b)


def add_(op, out, a, b, ws=0, sf=0):
    return dict(add_op=op, add_out=out, ws=ws, add_a=a, add_b=b, sf=sf)


def emit_slot(p, m, a, sig=SIG_NONE):
    if sig != SIG_NONE:
        for d in (m, a):
            if d and d.get('add_b') == imm(0) or (d and 'mul_b' in d and d['mul_b'][0] == 8):
                pass
        for d, k in ((m, 'mul_b'), (a, 'add_b')):
            if d and d.get(k) is not None and d[k][0] == 8:
                raise AssertionError('TMU signal cannot share a word with a small immediate')
    kw = dict(sig=sig)
    if m:
        kw.update(m)
    else:
        kw.update(mul_op=MUL_NOP, mul_out=W_NOP)
    if a:
        kw.update(a)
    else:
        kw.update(add_op=ADD_NOP, add_out=W_NOP, cond_add=COND_NEVER)
    if not m:
        kw.setdefault('cond_mul', COND_NEVER)
    p.emit(_ops(**kw))


def rep_all(src_acc):
    return add_or(37, acc(src_acc), acc(src_acc), ws=1)


def build(N, coalesced_a=False):
    ncols = N // 16
    ntiles = ncols * ncols
    assert ncols & (ncols - 1) == 0
    lg_ncols = ncols.bit_length() - 1
    A_STEP = 4
    B_STEP = N * 4
    S1 = (N * 4).bit_length() - 1
    SROWOFF = (16 * N * 4).bit_length() - 1
    COLOFF_MASK = (ncols - 1) * 64
    stride = (N - 16) * 4

    p = Prog()

    def e(*ws):
        for w in ws:
            p.emit(_ops(**w) if isinstance(w, dict) else w)

    # ---------------- prologue
    e(add_or(36, imm(1), imm(1)))                        # tmu_noswap
    e(add_or(U_N, areg(R_UNIF), areg(R_UNIF)))           # u0 = N
    e(add_or(U_C, areg(R_UNIF), areg(R_UNIF)))           # u1 = C bus base
    e(add_or(U_A, areg(R_UNIF), areg(R_UNIF)))           # u2 = A bus base
    e(add_or(U_B, areg(R_UNIF), areg(R_UNIF)))           # u3 = B bus base
    e(add_or(U4, areg(R_UNIF), areg(R_UNIF)))            # u4 = unused
    e(add_or(T, areg(R_UNIF), areg(R_UNIF)))             # u5 = first tile index
    e(add_or(TSTR_A, areg(R_UNIF), areg(R_UNIF)))        # u6 = tiles per QPU stride
    e(add_or(RL, areg(R_ELEM_NUM), areg(R_ELEM_NUM)))    # lane
    e(nop())
    e(add_or(TSTR, ws=1, a=areg(TSTR_A), b=areg(TSTR_A)))  # tile stride in file B
    e(add_or(VPMQ, breg(R_QPU_NUM), breg(R_QPU_NUM)))     # VPM row base = 4*qpu
    e(nop(), nop())
    e(add_(ADD_SHL, VPMQ, areg(VPMQ), imm(2)))           # 4*qpu
    e(add_(ADD_SHL, ALANE, ws=1, a=areg(RL), b=imm(2 if coalesced_a else S1)))
    e(add_(ADD_SHL, BLANE, ws=1, a=areg(RL), b=imm(2)))
    e(ldi(B_STEP, BST, ws=1))
    e(ldi(4 * N * 4, CSTEP, ws=1))
    e(nop(), nop())
    e(ldi(0xFFFFFFFF, NEG1, ws=1))
    e(ldi((1 << 32) - ntiles, NEGNT, ws=1))
    e(add_(ADD_SHL, VPMQ7, ws=1, a=areg(VPMQ), b=imm(7)))
    e(nop(), nop())

    # ---------------- tile loop
    p.label('tile')
    e(add_(ADD_ADD, SA, areg(T), (REGB, NEGNT), sf=1))       # t - ntiles
    p.branch_to('tbody', cond=BR_ALL_N_SET)                  # N set <=> t < ntiles
    e(nop(), nop(), nop())
    p.branch_to('tdone', cond=BR_ALWAYS)
    e(nop(), nop(), nop())
    p.label('tbody')
    e(add_(ADD_AND, SA, areg(T), imm(ncols - 1)))            # colblk
    e(add_(ADD_SHR, SA + 1, areg(T), imm(lg_ncols)))         # rowblk
    e(nop(), nop())
    e(add_(ADD_SHL, COLOFF, ws=1, a=areg(SA), b=imm(6)))     # colblk*64 B
    e(add_(ADD_SHL, ROWOFF, ws=1, a=areg(SA + 1), b=imm(SROWOFF)))
    e(nop(), nop())
    e(add_(ADD_ADD, AADR, areg(U_A), (REGB, ROWOFF)))
    e(add_(ADD_ADD, BADR, areg(U_B), (REGB, COLOFF)))
    e(add_(ADD_ADD, CADR, areg(U_C), (REGB, ROWOFF)))
    e(nop(), nop())
    e(add_(ADD_ADD, AADR, areg(AADR), (REGB, ALANE)))
    e(add_(ADD_ADD, BADR, areg(BADR), (REGB, BLANE)))
    e(add_(ADD_ADD, CADR, areg(CADR), (REGB, COLOFF)))
    e(nop(), nop())
    for r in range(16):                                  # C rows = 0
        e(add_or(RC + r, imm(0), imm(0)))
    e(nop(), nop())
    for j in range(2):                                   # prime 2 requests / TMU
        e(add_or(W_TMU0_S, areg(AADR), areg(AADR), ws=1))
        e(add_or(W_TMU1_S, areg(BADR), areg(BADR), ws=1))
        e(nop(), nop())
        e(add_(ADD_ADD, AADR, areg(AADR), imm(A_STEP)))
        e(add_(ADD_ADD, BADR, areg(BADR), (REGB, BST)))
        e(nop(), nop())
    e(ldi(N, KCNT))
    e(nop(), nop())

    # ---------------- k loop: 39 instructions per k
    # NOTE: a word that carries a small immediate cannot carry a TMU signal
    # (the ALU-immediate signal takes the signal field), so the pops live on
    # plain words.
    # TMU issue and TMU pop must not share one instruction word: a word that
    # writes a TMU address register *and* carries a load_tmu signal returns
    # corrupted data for the request it issues (measured, see NOTES.md).
    head = [
        (None, None, SIG_LOAD_TMU0),                                     # pop A_k -> r4
        (None, add_or(32, acc(4), acc(4)), SIG_NONE),                    # acc0 = A column
        (None, None, SIG_LOAD_TMU1),                                     # pop B_k -> r4
        (None, add_or(W_TMU0_S, areg(AADR), areg(AADR), ws=1), SIG_NONE),  # issue A
        (None, add_or(W_TMU1_S, areg(BADR), areg(BADR), ws=1), SIG_NONE),  # issue B
    ]
    core = [(mul_rot(1, 0, 1), rep_all(0), SIG_NONE)]
    core = [(mul_rot(1, 0, 15), rep_all(0), SIG_NONE)]        # rot(row1) ; rep(row0)
    for m in range(16):
        # broadcast of row m+1 comes from acc1, written by the rotation 2 slots ago
        nxt = rep_all(1) if m < 15 else None
        core.append((mul_fmul(2, acc(5), acc(4)), nxt, SIG_NONE))
        # this rotation prepares row m+2:  rep(rot(v,n)) == v[16-n]
        rot = mul_rot(1, 0, 14 - m) if m < 14 else None
        core.append((rot, add_(ADD_FADD, RC + m, areg(RC + m), acc(2)), SIG_NONE))
    # core[31] = (P15, None) -> counter ; core[32] = (None, A15) -> delay slot
    core[31] = (core[31][0], add_(ADD_ADD, KCNT, (REGB, NEG1), areg(KCNT), sf=1), SIG_NONE)

    p.label('kloop')
    for m, a, s in head:
        emit_slot(p, m, a, s)
    for m, a, s in core[:31]:
        emit_slot(p, m, a, s)
    emit_slot(p, *core[31][:2])                       # P15 | counter (Z set at 0)
    p.branch_to('kloop', cond=BR_ALL_Z_CLEAR)         # loop while counter != 0
    emit_slot(p, *core[32][:2])                       # A15 (delay slot 1)
    e(add_(ADD_ADD, AADR, areg(AADR), imm(A_STEP)))       # delay slot 2
    e(add_(ADD_ADD, BADR, areg(BADR), (REGB, BST)))       # delay slot 3

    # ---------------- drain 2+2 outstanding requests
    for _ in range(2):
        e(alu(sig=SIG_LOAD_TMU0, cond_add=COND_NEVER, cond_mul=COND_NEVER))
    for _ in range(2):
        e(alu(sig=SIG_LOAD_TMU1, cond_add=COND_NEVER, cond_mul=COND_NEVER))
    e(nop())

    # ---------------- store 16 rows as 4 chunks of 4
    dma = (2 << 30) | (4 << 23) | (16 << 16) | (1 << 14)
    for c in range(4):
        e(ldi(0x1A00, R5, ws=1))
        e(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1, add_a=acc(5), add_b=areg(VPMQ)))
        for r in range(4):
            e(add_or(W_VPM, areg(RC + 4 * c + r), areg(RC + 4 * c + r)))
        e(ldi(dma, R5, ws=1))
        e(_ops(add_op=ADD_ADD, add_out=W_VPW_SETUP, ws=1, add_a=acc(5), add_b=(REGB, VPMQ7)))
        e(ldi(0xC0010000 | stride, W_VPW_SETUP, ws=1))
        e(add_or(W_VPW_ADDR, areg(CADR), areg(CADR), ws=1))
        e(_ops(add_op=ADD_OR, add_out=W_NOP, add_a=(REGB, R_VPW_WAIT), add_b=(REGB, R_VPW_WAIT)))
        e(nop(), nop(), nop(), nop())          # let the VPM DMA drain fully
        e(add_(ADD_ADD, CADR, areg(CADR), (REGB, CSTEP)))
    e(add_(ADD_ADD, T, areg(T), (REGB, TSTR)))
    e(nop(), nop())
    p.branch_to('tile', cond=BR_ALWAYS)
    e(nop(), nop(), nop())

    p.label('tdone')
    e(end_program())
    e(nop(), nop(), nop())
    return p


if __name__ == '__main__':
    ASM = os.path.dirname(os.path.abspath(__file__))
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 128
    p = build(N)
    w = p.write(os.path.join(ASM, f'gemm_fixed{N}.bin'))
    print(f'gemm_fixed{N}.bin {len(w)} instructions')
