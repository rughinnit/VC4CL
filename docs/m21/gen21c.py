#!/usr/bin/env python3
"""M21c: lane-interleaved 4x4 tile with explicit column blocks.

Grid (2D): x = 16 lanes (merged), y = (N/R) row groups * (N/64) column blocks.
  lane   = get_global_id(0)
  yid    = get_global_id(1)
  block  = yid / (N/R)
  rowgrp = yid % (N/R)
  col    = block*64 + lane + 16*j     -> store step per lane is exactly 1 element

usage: gen21c.py <N> <kernel> <R> <C> <KB>
"""
import sys

N, KERNEL, R, C, KB = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
NL = 16

assert N % 64 == 0 and N % R == 0, "N must be divisible by 64 and R"

body = []
body.append("  int lane = get_global_id(0);")
body.append("  int yid  = get_global_id(1);")
body.append("  int blk  = yid / %d;" % (N // R))
body.append("  int rg   = yid %% %d;" % (N // R))
body.append("  int c0   = blk * %d + lane;" % (NL * C))
body.append("  int aBase = rg * (%d * N);" % R)
colref = ["c0 + %d" % (NL * j) for j in range(C)]

accs = ["r%dc%d" % (i, j) for i in range(R) for j in range(C)]
body.append("  float " + ", ".join("%s = 0.0f" % a for a in accs) + ";")
body.append("  for (int k = 0; k < N; k += %d)" % KB)
body.append("  {")

for i in range(R):
    if KB % 4 == 0:
        body.append("    float4 av%d = vload4(0, a + aBase + %d * N + k);" % (i, i))
for i in range(R):
    for kk in range(KB):
        if KB % 4 == 0:
            body.append("    float a%d_%d = av%d.%s;" % (i, kk, i, "xyzw"[kk % 4]))
        else:
            body.append("    float a%d_%d = a[aBase + %d * N + k + %d];" % (i, kk, i, kk))
for kk in range(KB):
    for j in range(C):
        body.append("    float b%d_%d = b[(k + %d) * N + %s];" % (kk, j, kk, colref[j]))
for i in range(R):
    for kk in range(KB):
        for j in range(C):
            body.append("    %s += a%d_%d * b%d_%d;" % (accs[i * C + j], i, kk, kk, j))
body.append("  }")
for i in range(R):
    for j in range(C):
        body.append("  c[aBase + %d * N + %s] = %s;" % (i, colref[j], accs[i * C + j]))
body.append("}")

hdr = ["#define N %d" % N,
       "__kernel void %s(__global const float* a, __global const float* b, __global float* c)" % KERNEL,
       "{"]
print("\n".join(hdr + body))
