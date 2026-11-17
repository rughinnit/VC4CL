#!/usr/bin/env python3
"""M21b: parameterised R x C register tile generator for the VC4 GEMM.

usage: gen21b.py <N> <kernel> <R> <C> <KB> <map> <loads>
  map   : nat (col = gid*C + j) | int (col = gid + 16*j)
  loads : vec (A via vload4 when KB%4==0) | sca (all scalar)
"""
import sys

N, KERNEL, R, C, KB = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
MAP, LOADS = sys.argv[6], sys.argv[7]
NL = 16
veca = (LOADS != "sca")

if MAP == "nat":
    body, colref = ["  int c0 = get_global_id(0) * %d;" % C], ["c0 + %d" % j for j in range(C)]
else:
    body, colref = ["  int c0 = get_global_id(0);"], ["c0 + %d" % (NL * j) for j in range(C)]

body.append("  int aBase = get_global_id(1) * (%d * N);" % R)
accs = ["r%dc%d" % (i, j) for i in range(R) for j in range(C)]
body.append("  float " + ", ".join("%s = 0.0f" % a for a in accs) + ";")
body.append("  for (int k = 0; k < N; k += %d)" % KB)
body.append("  {")

for i in range(R):
    if veca and KB % 4 == 0:
        body.append("    float4 av%d = vload4(0, a + aBase + %d * N + k);" % (i, i))
for i in range(R):
    for kk in range(KB):
        if veca and KB % 4 == 0:
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
