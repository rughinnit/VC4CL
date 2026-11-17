#!/usr/bin/env python3
"""M21f: general R x C tile with nat-map compute, lane-uniform A, per-lane B,
and gate-compatible interleaved stores (no compiler change needed).

Grid: x = 16 lanes, y = (N/R) row groups * (N/(16*C)) column blocks.
  cb = blk*(16*C) + lane*C       (compute columns, nat)
  ob = blk*(16*C) + lane          (store base, lane-major)
  value for compute column (lane*C + j) is stored at (ob + 16*j)
A later permute kernel restores row-major order.

usage: gen21f.py <N> <kernel> <R> <C> <KB>
"""
import sys

N, KERNEL, R, C, KB = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
BLK = 16 * C
assert N % BLK == 0 and N % R == 0

b = []
b.append("  int lane = get_global_id(0);")
b.append("  int yid  = get_global_id(1);")
b.append("  int blk  = yid / %d;" % (N // R))
b.append("  int rg   = yid %% %d;" % (N // R))
b.append("  int cb   = blk * %d + lane * %d;" % (BLK, C))
b.append("  int ob   = blk * %d + lane;" % BLK)
b.append("  int aBase = rg * (%d * N);" % R)
accs = ["r%dc%d" % (i, j) for i in range(R) for j in range(C)]
b.append("  float " + ", ".join("%s = 0.0f" % a for a in accs) + ";")
b.append("  for (int k = 0; k < N; k += %d)" % KB)
b.append("  {")
for i in range(R):
    if KB % 4 == 0:
        b.append("    float4 av%d = vload4(0, a + aBase + %d * N + k);" % (i, i))
for i in range(R):
    for kk in range(KB):
        if KB % 4 == 0:
            b.append("    float a%d_%d = av%d.%s;" % (i, kk, i, "xyzw"[kk % 4]))
        else:
            b.append("    float a%d_%d = a[aBase + %d * N + k + %d];" % (i, kk, i, kk))
for kk in range(KB):
    for j in range(C):
        b.append("    float b%d_%d = b[(k + %d) * N + cb + %d];" % (kk, j, kk, j))
for i in range(R):
    for kk in range(KB):
        for j in range(C):
            b.append("    %s += a%d_%d * b%d_%d;" % (accs[i * C + j], i, kk, kk, j))
b.append("  }")
for i in range(R):
    for j in range(C):
        b.append("  c[aBase + %d * N + ob + %d] = %s;" % (i, 16 * j, accs[i * C + j]))
b.append("}")

hdr = ["#define N %d" % N,
       "__kernel void %s(__global const float* a, __global const float* b, __global float* c)" % KERNEL,
       "{"]
print("\n".join(hdr + b))
