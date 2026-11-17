#!/usr/bin/env python3
"""M21e: nat-map compute (lane owns 4 adjacent columns) with gate-compatible
interleaved stores, so no compiler change is needed.

Grid: x = 16 lanes, y = (N/R) row groups * (N/64) column blocks.
  lane   = get_global_id(0)
  blk    = gid1 / (N/R); rg = gid1 % (N/R)
  compute columns: cb = blk*64 + lane*4     (nat, coalesced B)
  store position : ob = blk*64 + lane        (lane-major, step 1 per lane)
  stored value for column (lane*4 + j) lives at (blk*64 + lane + 16*j)

usage: gen21e.py <N> <kernel> <R> <KB> [vecb]
"""
import sys

N, KERNEL, R, KB = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
vecb = len(sys.argv) > 5 and sys.argv[5] == "1"
C = 4
assert N % 64 == 0 and N % R == 0

body = []
body.append("  int lane = get_global_id(0);")
body.append("  int yid  = get_global_id(1);")
body.append("  int blk  = yid / %d;" % (N // R))
body.append("  int rg   = yid %% %d;" % (N // R))
body.append("  int cb   = blk * 64 + lane * %d;" % C)
body.append("  int ob   = blk * 64 + lane;")
body.append("  int aBase = rg * (%d * N);" % R)
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
    if vecb:
        body.append("    float4 bv%d = vload4(0, b + (k + %d) * N + cb);" % (kk, kk))
    else:
        for j in range(C):
            body.append("    float b%d_%d = b[(k + %d) * N + cb + %d];" % (kk, j, kk, j))
for i in range(R):
    for kk in range(KB):
        for j in range(C):
            bname = ("bv%d.%s" % (kk, "xyzw"[j])) if vecb else ("b%d_%d" % (kk, j))
            body.append("    %s += a%d_%d * %s;" % (accs[i * C + j], i, kk, bname))
body.append("  }")
for i in range(R):
    for j in range(C):
        body.append("  c[aBase + %d * N + ob + %d] = %s;" % (i, 16 * j, accs[i * C + j]))
body.append("}")

hdr = ["#define N %d" % N,
       "__kernel void %s(__global const float* a, __global const float* b, __global float* c)" % KERNEL,
       "{"]
print("\n".join(hdr + body))
